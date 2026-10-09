#include "pipeline/network_runtime.h"
#include "pipeline/stream_codec.h"
#include "pipeline/engine_client.h"
#include "core/network/tcp_packet_socket.h"
#include "pipeline/port_delivery_codec.h"
#include "core/latest_delivery.h"
#include <desktop/graph_document.h>
#include <QTcpServer>
#include <QTimer>
#include <QDateTime>
#include <QDataStream>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QPointer>
#include <algorithm>
#include <map>
#include <utility>
#include <QElapsedTimer>
namespace pipeline {
namespace {
constexpr quint8 CommandId = 5, ReplyId = 6, SnapshotId = 7, ViewId = 8, AudioId = 9, FragmentId = 10,
                 TypedViewsId = 17;
constexpr qsizetype MaxMessage = 2 * 1024 * 1024, FragmentSize = 60000, MaxQueued = 2 * 1024 * 1024;
void select_view_fields(ViewFrame &frame, quint32 subscriptions) {
    if (!(subscriptions & EngineCommand::TypedViews)) frame.views.clear();
    if (!(subscriptions & EngineCommand::History)) frame.history.reset();
    if (!(subscriptions & EngineCommand::Previews)) frame.previews.clear();
    if (!(subscriptions & EngineCommand::Diagnostics)) {
        frame.window_signal.clear();frame.window.clear();frame.windowed_signal.clear();
    }
}
quint64 epoch() {
    return QRandomGenerator::global()->generate64();
}
void setup(QDataStream &s) {
    s.setVersion(QDataStream::Qt_6_0);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);
}
bool bounded_length(QDataStream &s, quint32 limit) {
    const auto bytes = s.device()->peek(4);
    if (bytes.size() != 4) {
        s.setStatus(QDataStream::ReadPastEnd);
        return false;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
    const quint32 n = quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
    if (n == 0xffffffff)
        return true; // QDataStream null string/byte array.
    if (n > limit || quint64(n) + 4 > quint64(s.device()->bytesAvailable())) {
        s.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    return true;
}
void read_string(QDataStream &s, QString &v, quint32 limit = 65536) {
    if (bounded_length(s, limit))
        s >> v;
}
void read_blob(QDataStream &s, QByteArray &v, quint32 limit = MaxMessage) {
    if (bounded_length(s, limit))
        s >> v;
}
void floats(QDataStream &s, const std::vector<float> &v) {
    s << quint32(v.size());
    for (auto f : v)
        s << f;
}
void read_floats(QDataStream &s, std::vector<float> &v) {
    quint32 n = 0;
    s >> n;
    if (n > 262144 || quint64(n) * 4 > quint64(s.device()->bytesAvailable())) {
        s.setStatus(QDataStream::ReadCorruptData);
        return;
    }
    v.resize(n);
    for (auto &f : v)
        s >> f;
}
void history(QDataStream &s, const std::shared_ptr<const HistorySnapshot> &h) {
    s << bool(h);
    if (!h)
        return;
    s << quint64(h->graph_revision) << quint32(h->values.size());
    for (const auto &v : h->values)
        floats(s, v);
}
bool compatible_history(const std::shared_ptr<const HistorySnapshot> &h, quint64 graph, quint64 generation) {
    return h && h->graph_revision == graph && h->analyzer_generation == generation;
}
void read_history(QDataStream &s, std::shared_ptr<const HistorySnapshot> &h, quint64 generation) {
    bool has = false;
    s >> has;
    if (!has) {
        h.reset();
        return;
    }
    auto x = std::make_shared<HistorySnapshot>();
    quint64 r = 0;
    quint32 n = 0;
    s >> r >> n;
    if (n > 2000) {
        s.setStatus(QDataStream::ReadCorruptData);
        return;
    }
    x->graph_revision = r;
    // Protocol v1 supplies the origin generation in the enclosing snapshot/view.
    x->analyzer_generation = generation;
    x->values.resize(n);
    for (auto &v : x->values)
        read_floats(s, v);
    h = std::move(x);
}
void snapshot_codec(QDataStream &s, const EngineSnapshot &v) {
    s << v.epoch << v.revision << v.graph_revision << v.analyzer_generation << v.capture_generation
      << QJsonDocument(v.graph).toJson(QJsonDocument::Compact) << quint32(v.statuses.size());
    for (auto i = v.statuses.begin(); i != v.statuses.end(); ++i)
        s << i.key() << quint8(i->state) << i->error << quint64(i->revision);
    history(s, compatible_history(v.history, v.graph_revision, v.analyzer_generation) ? v.history : nullptr);
}
void read_snapshot(QDataStream &s, EngineSnapshot &v) {
    QByteArray graph;
    quint32 n = 0;
    s >> v.epoch >> v.revision >> v.graph_revision >> v.analyzer_generation >> v.capture_generation;
    read_blob(s, graph);
    s >> n;
    if (n > 16) {
        s.setStatus(QDataStream::ReadCorruptData);
        return;
    }
    auto doc = QJsonDocument::fromJson(graph);
    if (!doc.isObject()) {
        s.setStatus(QDataStream::ReadCorruptData);
        return;
    }
    v.graph = doc.object();
    for (quint32 j = 0; j < n; ++j) {
        QString k, e;
        quint8 state = 0;
        quint64 r = 0;
        read_string(s, k, 128);
        s >> state;
        read_string(s, e);
        s >> r;
        v.statuses[k] = {static_cast<State>(state), e, r};
    }
    read_history(s, v.history, v.analyzer_generation);
}
void audio(QDataStream &s, const AudioFrame &v) {
    s << quint64(v.sequence) << qint64(v.timestamp_ns) << quint64(v.sample_rate) << quint64(v.generation)
      << v.discontinuity;
    floats(s, v.left);
    floats(s, v.right);
}
void read_audio(QDataStream &s, AudioFrame &v) {
    quint64 rate = 0;
    quint64 sequence = 0, generation = 0;
    qint64 timestamp = 0;
    s >> sequence >> timestamp >> rate >> generation >> v.discontinuity;
    v.sequence = sequence;
    v.timestamp_ns = timestamp;
    v.generation = generation;
    v.sample_rate = rate;
    read_floats(s, v.left);
    read_floats(s, v.right);
}
void view(QDataStream &s, const ViewFrame &v) {
    const auto &f = v.spectrum;
    s << quint64(v.epoch) << quint64(v.graph_revision) << quint64(v.ready_timestamp_ns) << v.spectrogram
      << quint64(f.sequence) << qint64(f.timestamp_ns) << qint64(f.input_timestamp_ns) << quint64(f.audio_samples)
      << f.audio_peak << quint64(f.sample_rate) << quint64(f.generation) << f.discontinuity
      << quint64(f.dropped_windows);
    for (auto p : {&f.left, &f.right, &f.mel_left, &f.mel_right, &v.fft, &v.mel, &v.window_signal, &v.window,
                   &v.windowed_signal})
        floats(s, *p);
    s << v.frequency_step << quint32(v.previews.size());
    for (auto i = v.previews.begin(); i != v.previews.end(); ++i)
        s << qint32(i.key()) << quint64(i->values) << i->peak;
    history(s, compatible_history(v.history, v.graph_revision, f.generation) ? v.history : nullptr);
}
void read_view(QDataStream &s, ViewFrame &v) {
    auto &f = v.spectrum;
    quint64 samples = 0, rate = 0;
    quint64 epoch_value = 0, graph_revision = 0, ready = 0, sequence = 0, generation = 0, dropped = 0;
    qint64 timestamp = 0, input_timestamp = 0;
    s >> epoch_value >> graph_revision >> ready >> v.spectrogram >> sequence >> timestamp >> input_timestamp >>
        samples >> f.audio_peak >> rate >> generation >> f.discontinuity >> dropped;
    v.epoch = epoch_value;
    v.graph_revision = graph_revision;
    v.ready_timestamp_ns = ready;
    f.sequence = sequence;
    f.timestamp_ns = timestamp;
    f.input_timestamp_ns = input_timestamp;
    f.generation = generation;
    f.dropped_windows = dropped;
    f.audio_samples = samples;
    f.sample_rate = rate;
    for (auto p : {&f.left, &f.right, &f.mel_left, &f.mel_right, &v.fft, &v.mel, &v.window_signal, &v.window,
                   &v.windowed_signal})
        read_floats(s, *p);
    quint32 n = 0;
    s >> v.frequency_step >> n;
    if (n > 128) {
        s.setStatus(QDataStream::ReadCorruptData);
        return;
    }
    for (quint32 j = 0; j < n; ++j) {
        qint32 id = 0;
        quint64 count = 0;
        float peak = 0;
        s >> id >> count >> peak;
        v.previews[id] = {size_t(count), peak};
    }
    read_history(s, v.history, f.generation);
}
template <class F> QByteArray encode(F f) {
    QByteArray b;
    QDataStream s(&b, QIODevice::WriteOnly);
    setup(s);
    s << quint16(1);
    f(s);
    return b;
}
template <class F> bool decode(const QByteArray &b, F f) {
    if (b.size() > MaxMessage)
        return false;
    QDataStream s(b);
    setup(s);
    quint16 version;
    s >> version;
    if (version != 1)
        return false;
    f(s);
    return s.status() == QDataStream::Ok && s.atEnd();
}
void put32(QByteArray &b, quint32 v) {
    for (int i = 0; i < 4; ++i)
        b.append(char(v >> (8 * i)));
}
void put64(QByteArray &b, quint64 v) {
    for (int i = 0; i < 8; ++i)
        b.append(char(v >> (8 * i)));
}
quint64 getLE(const QByteArray &b, qsizetype &p, int n) {
    quint64 v = 0;
    for (int i = 0; i < n; ++i)
        v |= quint64(quint8(b[p++])) << (8 * i);
    return v;
}
QByteArray typed_views(const ViewFrame &v) {
    QByteArray b;
    b.append(char(2));
    b.append(char(0));
    put64(b, v.epoch);
    put64(b, v.graph_revision);
    put32(b, v.views.size());
    for (auto i = v.views.begin(); i != v.views.end(); ++i) {
        auto f = i.value();
        f.epoch = v.epoch;
        f.graph_revision = v.graph_revision;
        if (!stream::valid(f))
            return {};
        auto bytes = stream::encode(f);
        put32(b, i.key());
        put32(b, bytes.size());
        b.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (b.size() > MaxMessage)
            return {};
    }
    return b;
}
bool read_typed_views(const QByteArray &b, ViewFrame &v) {
    if (b.size() < 22 || b.size() > MaxMessage)
        return false;
    qsizetype p = 0;
    if (getLE(b, p, 2) != 2)
        return false;
    v.epoch = getLE(b, p, 8);
    v.graph_revision = getLE(b, p, 8);
    auto n = getLE(b, p, 4);
    if (n > 128)
        return false;
    for (quint64 i = 0; i < n; ++i) {
        if (b.size() - p < 8)
            return false;
        int id = getLE(b, p, 4);
        auto length = getLE(b, p, 4);
        if (length > quint64(b.size() - p))
            return false;
        TypedFrame f;
        std::string error;
        if (!stream::decode({reinterpret_cast<const uint8_t *>(b.constData() + p), size_t(length)}, f, error) ||
            f.epoch != v.epoch || f.graph_revision != v.graph_revision || v.views.contains(id))
            return false;
        p += length;
        v.views[id] = std::move(f);
    }
    return p == b.size();
}
EngineSnapshot collect(AudioPipeline &e, quint64 ep, quint64 rev) {
    EngineSnapshot v;
    v.epoch = ep;
    v.revision = rev;
    v.graph = e.graph();
    v.graph_revision = e.graph_revision();
    v.analyzer_generation = e.analyzer_generation();
    v.capture_generation = e.capture_generation();
    for (auto k : {"Analyzer", "Capture", "WLED"})
        v.statuses[k] = e.current_status(k);
    v.history = e.latest_history();
    if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation) ||
        v.statuses.value("Analyzer").state != State::Running)
        v.history.reset();
    return v;
}
EngineReply execute(AudioPipeline &e, const EngineCommand &c, quint64 id, quint64 ep, quint64 &rev) {
    EngineReply r;
    r.request_id = id;
    if (c.kind != EngineCommand::Snapshot && c.kind != EngineCommand::Subscribe &&
        c.expected_revision != std::numeric_limits<quint64>::max() && c.expected_revision != rev)
        r.error = "Revision conflict";
    else
        try {
            r.accepted = true;
            switch (c.kind) {
            case EngineCommand::Snapshot:
            case EngineCommand::Subscribe:
                break;
            case EngineCommand::ApplyGraph:
                r.accepted = e.apply_graph(c.graph, r.error);
                break;
            default:
                r.accepted = false;
                r.error = "Unknown command";
            }
            if (r.accepted && c.kind != EngineCommand::Snapshot && c.kind != EngineCommand::Subscribe)
                ++rev;
        } catch (const std::exception &x) {
            r.accepted = false;
            r.error = x.what();
        }
    r.snapshot = collect(e, ep, rev);
    return r;
}
class PortMailbox : public std::enable_shared_from_this<PortMailbox> {
    QObject *owner;
    std::mutex mutex;
    std::map<EndpointIdentity,DeliveredPortFrame> pending;
    size_t bytes=0;
    bool failed=false,cancelled=false;
    std::function<void(std::vector<DeliveredPortFrame>,bool)> handler;
public:
    PortMailbox(QObject *o,std::function<void(std::vector<DeliveredPortFrame>,bool)> h):owner(o),handler(std::move(h)){}
    void publish(DeliveredPortFrame value) {
        std::lock_guard lock(mutex);if(cancelled)return;
        const auto size=stream::payload_size(value.frame)+value.endpoint.graph_id.size()+128;
        const auto old=pending.find(value.endpoint);const auto previous=old==pending.end()?0:stream::payload_size(old->second.frame)+old->second.endpoint.graph_id.size()+128;
        if(size>2*1024*1024 || bytes-previous>2*1024*1024-size || (old==pending.end() && pending.size()>=2048)){failed=true;return;}
        bytes=bytes-previous+size;pending[value.endpoint]=std::move(value);
    }
    void start() {
        auto timer=new QTimer(owner);timer->setInterval(17);
        QObject::connect(timer,&QTimer::timeout,owner,[weak=weak_from_this()] {
            const auto self=weak.lock();if(!self)return;
            std::vector<DeliveredPortFrame> values;bool overflow=false;
            {std::lock_guard lock(self->mutex);if(self->cancelled)return;
                values.reserve(self->pending.size());for(auto &[key,value]:self->pending)values.push_back(std::move(value));
                self->pending.clear();self->bytes=0;overflow=std::exchange(self->failed,false);}
            if(!values.empty() || overflow)self->handler(std::move(values),overflow);
        });timer->start();
    }
    void cancel(){std::lock_guard lock(mutex);cancelled=true;pending.clear();bytes=0;}
};
// One bounded assembly per socket. TCP preserves fragment order. A new message cannot interleave.
struct Wire {
    TcpBDSPSocket *socket;
    quint64 serial = 0, receiving = 0;
    quint8 kind = 0;
    quint32 total = 0;
    QByteArray partial;
    quint32 subscriptions =
        EngineCommand::Views | EngineCommand::History | EngineCommand::Previews | EngineCommand::TypedViews;
    std::shared_ptr<const HistorySnapshot> last_history;
    bool canonical=false;
    uint64_t port_nonce=0,port_last_nonce=0,port_session=0,port_sequence=0,port_revision=0,port_epoch=0;
    std::string port_graph;
    port_delivery::Subscribe port_subscription;
    std::vector<DeliveredPortFrame> port_markers;
    std::vector<port_delivery::PortBinding> port_admitted;
    QElapsedTimer assembly_time;
    void reset_assembly() noexcept {receiving=0;total=0;kind=0;partial=QByteArray{};assembly_time.invalidate();}
    bool send(quint8 id, const QByteArray &b) {
        const auto bound=stream::fragmented_message_bound(size_t(b.size()),23,true);
        if(socket->bytesToWrite()<0 || !stream::peer_budget_fits(size_t(socket->bytesToWrite()),socket->pending_packet_bytes(),receiving?total:0,bound)){reset_assembly();socket->abort();return false;}
        if(b.size()<=65535) {if(socket->send_packet(id,b))return true;reset_assembly();socket->abort();return false;}
        if(serial==UINT64_MAX){reset_assembly();socket->abort();return false;}
        const auto message=++serial;
        for(qsizetype off=0;off<b.size();off+=FragmentSize) {
            auto fragment=encode([&](QDataStream &s){s<<id<<message<<quint32(b.size())<<quint32(off)<<b.mid(off,FragmentSize);});
            if(!socket->send_packet(FragmentId,fragment)){reset_assembly();socket->abort();return false;}
        }
        return true;
    }
    bool receive(quint8 &id, QByteArray &b) {
        const auto reject=[&]{reset_assembly();socket->abort();return false;};
        if (id != FragmentId) {
            if(receiving || b.size()>65535)return reject();
            return true;
        }
        quint8 k = 0;
        quint64 seq = 0;
        quint32 n = 0, off = 0;
        QByteArray chunk;
        // Fragment v1 is fixed-width through its QByteArray length. Inspect the
        // complete geometry before QDataStream allocates the chunk.
        const auto u32=[&](int p){const auto *x=reinterpret_cast<const uint8_t*>(b.constData()+p);return uint32_t(x[0])|(uint32_t(x[1])<<8)|(uint32_t(x[2])<<16)|(uint32_t(x[3])<<24);};
        if(b.size()<23 || uint8_t(b[0])!=1 || uint8_t(b[1])!=0) {return reject();}
        const auto bytes=u32(19),length=u32(11),offset=u32(15);
        const auto fragment_kind=uint8_t(b[2]);uint64_t fragment_sequence=0;
        for(int i=0;i<8;++i)fragment_sequence|=uint64_t(uint8_t(b[3+i]))<<(i*8);
        if(!bytes || bytes>FragmentSize || b.size()!=qsizetype(23)+bytes || length>MaxMessage || length<=65535 ||
           offset>length || bytes>length-offset || (socket->bytesToWrite()<0 || !stream::peer_budget_fits(size_t(socket->bytesToWrite()),socket->pending_packet_bytes(),length,0)) || !fragment_sequence ||
           ((fragment_kind<CommandId || fragment_kind>AudioId) && fragment_kind!=TypedViewsId && !(canonical && fragment_kind==port_delivery::FramesId)) ||
           (!offset && receiving) || (offset && (!receiving || fragment_sequence!=receiving || fragment_kind!=kind || length!=total || offset!=uint32_t(partial.size()) || assembly_time.elapsed()>=5000))) {return reject();}
        if (!decode(b,
                    [&](QDataStream &s) {
                        s >> k >> seq >> n >> off;
                        read_blob(s, chunk, FragmentSize);
                    }) ||
            ((k < CommandId || k > AudioId) && k != TypedViewsId && !(canonical && k==port_delivery::FramesId)) || n > MaxMessage || n <= 65535 ||
            chunk.isEmpty() || chunk.size() > FragmentSize || off>n || quint64(chunk.size())>quint64(n)-off ||
            (socket->bytesToWrite()<0 || !stream::peer_budget_fits(size_t(socket->bytesToWrite()),socket->pending_packet_bytes(),n,0))) {
            return reject();
        }
        if (off == 0) {
            if(receiving){return reject();}
            receiving = seq;
            kind = k;
            total = n;
            partial.clear();
            partial.reserve(n);
            assembly_time.start();
        }
        if (!receiving || !seq || assembly_time.elapsed()>=5000 || seq != receiving || k != kind || n != total || off != quint32(partial.size())) {
            return reject();
        }
        partial.append(chunk);
        if (partial.size() != total)
            return false;
        id = kind;
        b = std::move(partial);
        receiving = 0;
        total=0;kind=0;assembly_time.invalidate();
        return true;
    }
};
} // namespace
class EngineClient::Impl {
  public:
    EngineClient *owner;
    std::unique_ptr<AudioPipeline> engine;
    std::unique_ptr<NetworkRuntime> network;
    QString network_warning;
    QThread commands;
    QObject *worker = nullptr;
    QString host;
    quint16 port = 0;
    TcpBDSPSocket *socket = nullptr;
    std::unique_ptr<Wire> wire;
    QTimer retry;
    int delay = 200;
    bool enabled = false;
    quint64 next = 0, ep = epoch(), rev = 0;
    std::atomic<size_t> outstanding{0};
    std::atomic<uint64_t> view_subscription_generation{0}, audio_subscription_generation{0};
    quint32 subscriptions =
        EngineCommand::Views | EngineCommand::History | EngineCommand::Previews | EngineCommand::TypedViews;
    EngineSnapshot cache;
    QJsonObject presentation_graph;
    QMap<quint64, qint64> pending;
    std::function<void(EngineSnapshot)> snapshot_handler;
    std::function<void(ViewFrame)> view_handler;
    std::function<void(AudioFrame)> audio_handler;
    std::function<void(EngineReply)> reply_handler;
    std::function<void(DeliveredPortFrame)> port_handler;
    std::shared_ptr<PortMailbox> ports;
    uint64_t port_token=0,port_nonce=0,last_port_sequence=0;
    qint64 port_requested_at=0;
    bool port_requested=false;
    EnginePort::PortDeliveryMode port_mode=EnginePort::PortDeliveryMode::Unsupported;
    port_delivery::Subscribe port_request;
    void deliver_ports(std::vector<DeliveredPortFrame> values,bool snapshot) {
        if(snapshot)cache.port_frames.clear();
        for(auto &value:values) {
            const auto found=std::find_if(cache.port_frames.begin(),cache.port_frames.end(),[&](const auto &f){return f.endpoint==value.endpoint;});
            if(found==cache.port_frames.end())cache.port_frames.push_back(value);else *found=value;
            if(port_handler)port_handler(std::move(value));
        }
    }
    bool request_ports(std::vector<EndpointIdentity> selected,QString *error) {
        if(engine) {
            const auto token=engine->subscribe_ports([mailbox=ports](DeliveredPortFrame value){mailbox->publish(std::move(value));},selected);
            if(!token){if(error)*error="Invalid canonical subscription";return false;}
            if(port_token)engine->unsubscribe_ports(port_token);
            port_token=token;port_request.endpoints=selected;cache.port_frames=engine->latest_port_frames(selected);port_mode=EnginePort::PortDeliveryMode::CanonicalPortsV1;return true;
        }
        if(!socket || socket->state()!=QAbstractSocket::ConnectedState){if(error)*error="Canonical negotiation requires connection";return false;}
        if(port_nonce==UINT64_MAX){if(error)*error="Canonical nonce exhausted; reconnect required";return false;}
        port_request.endpoints=std::move(selected);port_request.nonce=++port_nonce;
        port_request.limits.max_message_bytes=uint32_t(MaxMessage-2048);
        std::vector<uint8_t> bytes(MaxMessage);
        auto result=port_delivery::encode(port_request,bytes);
        if(!result){if(error)*error="Invalid canonical subscription";return false;}
        port_requested=true;port_requested_at=QDateTime::currentMSecsSinceEpoch();port_mode=EnginePort::PortDeliveryMode::Unsupported;
        wire->send(port_delivery::SubscribeId,QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(result.written_bytes)));
        return true;
    }
    using LocalView = std::pair<ViewFrame, uint64_t>;
    using LocalAudio = std::pair<AudioFrame, uint64_t>;
    std::shared_ptr<LatestDelivery<LocalView>> views;
    std::shared_ptr<LatestDelivery<LocalAudio>> audios;
    std::shared_ptr<LatestDelivery<bool>> statuses;
    Impl(EngineClient *o) : owner(o) {
        retry.setSingleShot(true);
        QObject::connect(&retry, &QTimer::timeout, o, [this] { connect(); });
        auto *expiry = new QTimer(o);
        expiry->setInterval(100);
        QObject::connect(expiry, &QTimer::timeout, o, [this] {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if(wire && wire->receiving && wire->assembly_time.elapsed()>=5000)socket->abort();
            if(port_requested && now-port_requested_at>=5000) {
                port_requested=false;connection(State::Failed,"Canonical port delivery negotiation timed out");
                enabled=false;socket->abort();
            }
            for (auto id : pending.keys())
                if (pending.contains(id) && now - pending.value(id) >= 5000) {
                    pending.remove(id);
                    if (reply_handler)
                        reply_handler({id, false, "Command timed out", cache});
                    if (socket && cache.epoch == 0) {
                        connection(State::Recovering, "Engine handshake timed out");
                        socket->abort();
                        break;
                    }
                }
        });
        expiry->start();
    }
    void connection(State state, QString error = {}) {
        cache.statuses["Connection"] = {state, std::move(error), cache.statuses.value("Connection").revision + 1};
        if (snapshot_handler)
            snapshot_handler(cache);
    }
    void publish(EngineSnapshot v) {
        if(socket && !presentation_graph.isEmpty())v.graph=desktop::restore_graph_layout(v.graph,presentation_graph);
        // Validate against the original generation before monotonic state merging.
        if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation))
            v.history.reset();
        if (socket) {
            auto status = cache.statuses.value("Connection");
            if (v.epoch != 0) {
                status.state = State::Running;
                status.error.clear();
                ++status.revision;
            }
            v.statuses["Connection"] = status;
        }
        if (v.epoch == cache.epoch && v.epoch != 0) {
            for (auto i = cache.statuses.begin(); i != cache.statuses.end(); ++i)
                if (i->revision > v.statuses.value(i.key()).revision)
                    v.statuses[i.key()] = i.value();
            if (v.revision < cache.revision || v.graph_revision < cache.graph_revision) {
                v.graph = cache.graph;
                v.graph_revision = cache.graph_revision;
            }
            v.revision = std::max(v.revision, cache.revision);
            v.analyzer_generation = std::max(v.analyzer_generation, cache.analyzer_generation);
            v.capture_generation = std::max(v.capture_generation, cache.capture_generation);
            if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation))
                v.history.reset();
            if (!v.history && compatible_history(cache.history, v.graph_revision, v.analyzer_generation) &&
                v.statuses.value("Analyzer").state == State::Running)
                v.history = cache.history;
        }
        if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation) ||
            v.statuses.value("Analyzer").state != State::Running)
            v.history.reset();
        if(engine && port_token)v.port_frames=engine->latest_port_frames(port_request.endpoints);
        else if(v.epoch==cache.epoch)v.port_frames=std::move(cache.port_frames);
        cache = std::move(v);
        if (snapshot_handler)
            snapshot_handler(cache);
    }
    void reply(EngineReply r) {
        if (!pending.remove(r.request_id))
            return;
        publish(r.snapshot);
        r.snapshot = cache;
        if (reply_handler)
            reply_handler(std::move(r));
    }
    void local(AudioSourceFactory source_factory) {
        engine = std::make_unique<AudioPipeline>(std::move(source_factory));
        network = std::make_unique<NetworkRuntime>(*engine);
        worker = new QObject;
        worker->moveToThread(&commands);
        QObject::connect(&commands, &QThread::finished, worker, &QObject::deleteLater);
        commands.start();
        cache = collect(*engine, ep, rev);
        port_mode=EnginePort::PortDeliveryMode::CanonicalPortsV1;
        ports=std::make_shared<PortMailbox>(owner,[this](std::vector<DeliveredPortFrame> values,bool overflow) {
            if(overflow){connection(State::Failed,"Canonical port mailbox exhausted");return;}
            std::erase_if(values,[&](const auto &f){return f.delivery.epoch!=engine->epoch() || f.delivery.graph_revision!=engine->graph_revision();});
            deliver_ports(std::move(values),false);
        });ports->start();
        views = std::make_shared<LatestDelivery<LocalView>>(owner, [this](LocalView queued) {
            if (!(subscriptions & EngineCommand::Views) || queued.second != view_subscription_generation.load())
                return;
            auto v=std::move(queued.first);
            if (v.epoch != engine->epoch() || v.graph_revision != engine->graph_revision())
                return;
            if (subscriptions & EngineCommand::TypedViews) v.views=engine->latest_views();
            select_view_fields(v,subscriptions);
            v.epoch = ep;
            if (view_handler)
                view_handler(std::move(v));
        });
        audios = std::make_shared<LatestDelivery<LocalAudio>>(owner, [this](LocalAudio queued) {
            if (!(subscriptions & EngineCommand::Audio) || queued.second != audio_subscription_generation.load())
                return;
            auto v=std::move(queued.first);
            if (v.generation != engine->capture_generation() ||
                engine->current_status("Capture").state != State::Running)
                return;
            if (audio_handler)
                audio_handler(std::move(v));
        });
        views->start_polling();
        audios->start_polling();
        engine->set_spectrum_handler([this, delivery = views](ViewFrame v) {
            delivery->publish({std::move(v),view_subscription_generation.load()});
        });
        engine->set_audio_handler([this, delivery = audios](AudioFrame v) {
            delivery->publish({std::move(v),audio_subscription_generation.load()});
        });
        statuses = std::make_shared<LatestDelivery<bool>>(owner, [this](bool) {
            auto updated = cache;
            updated.analyzer_generation = engine->analyzer_generation();
            updated.capture_generation = engine->capture_generation();
            updated.graph = engine->graph();
            updated.graph_revision = engine->graph_revision();
            for (auto k : {"Analyzer", "Capture", "WLED"})
                updated.statuses[k] = engine->current_status(k);
            publish(std::move(updated));
        });
        engine->set_status_handler([delivery = statuses](QString, ComponentStatus) { delivery->publish(true); });
    }
    void remote() {
        socket = new TcpBDSPSocket(owner);
        wire = std::make_unique<Wire>(Wire{socket});
        socket->setReadBufferSize(MaxMessage);
        QObject::connect(socket, &QTcpSocket::connected, owner, [this] {
            delay = 200;
            connection(State::Starting);
            EngineCommand c;
            owner->submit(c);
            c.kind = EngineCommand::Subscribe;
            c.subscriptions = subscriptions;
            owner->submit(c);
        });
        QObject::connect(socket, &QTcpSocket::disconnected, owner, [this] {
            wire->reset_assembly();
            wire->canonical=false;port_mode=EnginePort::PortDeliveryMode::Unsupported;port_requested=false;
            last_port_sequence=0;port_nonce=0;cache.port_frames.clear();
            for (auto id : pending.keys()) {
                if (reply_handler)
                    reply_handler({id, false, "Disconnected", cache});
            }
            pending.clear();
            cache.epoch = 0;
            cache.history.reset();
            if (cache.statuses.value("Connection").state != State::Failed)
                connection(enabled ? State::Recovering : State::Stopped, "Disconnected");
            if (enabled) {
                retry.start(delay);
                delay = std::min(delay * 2, 5000);
            }
        });
        QObject::connect(socket, &QTcpSocket::errorOccurred, owner, [this](auto) {
            if (cache.statuses.value("Connection").state != State::Failed)
                connection(State::Recovering, socket->errorString());
            if (enabled && !retry.isActive()) {
                retry.start(delay);
                delay = std::min(delay * 2, 5000);
            }
        });
        QObject::connect(socket, &TcpBDSPSocket::on_bdsp_packet_parsing_error, owner, [this](auto, auto *) {
            connection(State::Recovering, "Invalid BDSP frame");
            socket->abort();
        });
        QObject::connect(socket, &TcpBDSPSocket::packetReceived, owner, [this](quint8 id, QByteArray b) {
            if (!wire->receive(id, b))
                return;
            bool ok = true;
            if(id==port_delivery::AckId) {
                port_delivery::Ack ack;
                ok=bool(port_delivery::decode({reinterpret_cast<const uint8_t*>(b.constData()),size_t(b.size())},ack));
                if(!port_requested || ack.nonce!=port_request.nonce)ok=false;
                if(ok && ack.status!=port_delivery::Status::Accepted) {
                    port_requested=false;port_mode=wire->canonical?EnginePort::PortDeliveryMode::CanonicalPortsV1:EnginePort::PortDeliveryMode::Unsupported;
                    connection(State::Failed,QString::fromStdString(ack.error));return;
                }
                if(ok && ((ack.type_mask&port_request.type_mask)!=port_request.type_mask || ack.limits.max_endpoints<port_request.endpoints.size() ||
                          ack.limits.max_message_bytes>port_request.limits.max_message_bytes || ack.limits.max_endpoints>port_request.limits.max_endpoints))ok=false;
                if(ok){wire->canonical=true;wire->port_nonce=ack.nonce;wire->port_session=ack.session_epoch;wire->port_subscription=port_request;
                    wire->port_subscription.limits=ack.limits;wire->port_subscription.type_mask=ack.type_mask;wire->port_graph=ack.graph_id;
                    wire->port_admitted.clear();
                    last_port_sequence=0;wire->port_revision=0;wire->port_epoch=0;port_requested=false;port_mode=EnginePort::PortDeliveryMode::CanonicalPortsV1;}
            } else if(id==port_delivery::FramesId) {
                port_delivery::Frames frames;
                ok=wire->canonical && bool(port_delivery::decode({reinterpret_cast<const uint8_t*>(b.constData()),size_t(b.size())},frames,wire->port_subscription.limits));
                if(ok && (frames.nonce!=wire->port_nonce || frames.session_epoch!=wire->port_session || frames.sequence<=last_port_sequence ||
                    frames.delivery.graph_revision<wire->port_revision || frames.delivery.epoch<wire->port_epoch))return;
                if(wire->canonical && !ok)return; // Reject malformed active batches without clearing accepted state.
                if(ok && (frames.graph_id!=wire->port_graph || (!last_port_sequence && !frames.snapshot)))ok=false;
                if(ok && frames.snapshot && !wire->port_subscription.endpoints.empty() &&
                   frames.entries.size()!=wire->port_subscription.endpoints.size())ok=false;
                if(ok)for(const auto &entry:frames.entries) {
                    const auto &selected=wire->port_subscription.endpoints;
                    if(!(wire->port_subscription.type_mask&(uint16_t(1)<<uint8_t(entry.frame.type))) ||
                       (!selected.empty() && std::find(selected.begin(),selected.end(),entry.endpoint)==selected.end())){ok=false;break;}
                }
                const auto graph=desktop::read_graph_document(cache.graph);
                std::vector<port_delivery::PortBinding> admitted;
                const bool configured=cache.graph.contains("nodes");
                if(configured) {
                    if(!graph.error.isEmpty())ok=false;
                    for(const auto &node:graph.graph.nodes) {
                        for(uint16_t port=0;port<domain::output_ports(node);++port)if(const auto type=domain::output_type(node,port))admitted.push_back({{frames.graph_id,node.id,PortDirection::Output,port},node.kind,*type});
                        if(domain::node_role(node.kind)==domain::NodeRole::Sink)for(uint16_t port=0;port<domain::input_ports(node);++port)if(const auto type=domain::input_type(node,port))admitted.push_back({{frames.graph_id,node.id,PortDirection::Input,port},node.kind,*type});
                    }
                } else if(last_port_sequence)admitted=wire->port_admitted;
                else if(ok && frames.snapshot)for(const auto &entry:frames.entries) {
                    const port_delivery::PortBinding binding{entry.endpoint,entry.kind,entry.frame.type};
                    if(!port_delivery::valid_bootstrap_binding(binding)){ok=false;break;}
                    admitted.push_back(binding);
                }
                if(!wire->port_subscription.endpoints.empty()) {
                    std::vector<port_delivery::PortBinding> selected;
                    for(const auto &endpoint:wire->port_subscription.endpoints) {
                        const auto binding=std::find_if(admitted.begin(),admitted.end(),[&](const auto &value){return value.endpoint==endpoint;});
                        if(binding!=admitted.end())selected.push_back(*binding);
                        else {
                            const auto previous=std::find_if(wire->port_admitted.begin(),wire->port_admitted.end(),[&](const auto &value){return value.endpoint==endpoint;});
                            if(previous==wire->port_admitted.end()){ok=false;break;}
                            auto marker=*previous;marker.unavailable_only=true;selected.push_back(std::move(marker));
                        }
                    }
                    admitted=std::move(selected);
                }
                if(ok && !port_delivery::validate_admitted_membership(frames,admitted))ok=false;
                if(ok)for(const auto &entry:frames.entries) {
                    const auto previous=std::find_if(cache.port_frames.begin(),cache.port_frames.end(),[&](const auto &value){return value.endpoint==entry.endpoint;});
                    if(previous!=cache.port_frames.end() && previous->delivery.graph_revision==frames.delivery.graph_revision &&
                       (previous->kind!=entry.kind || previous->frame.type!=entry.frame.type)){ok=false;break;}
                }
                if(wire->canonical && !ok)return;
                if(ok) {
                    const bool stopped_data=cache.statuses.value("Analyzer").state==State::Stopped &&
                        std::any_of(frames.entries.begin(),frames.entries.end(),[](const auto &value){return value.frame.available;});
                    if(stopped_data || frames.delivery.graph_revision<cache.graph_revision)return;
                    wire->port_admitted=std::move(admitted);
                    last_port_sequence=frames.sequence;
                    wire->port_revision=frames.delivery.graph_revision;wire->port_epoch=frames.delivery.epoch;
                    deliver_ports(std::move(frames.entries),frames.snapshot);
                }
            } else if (id == ReplyId) {
                EngineReply r;
                ok = decode(b, [&](QDataStream &s) {
                    s >> r.request_id >> r.accepted;
                    read_string(s, r.error);
                    read_snapshot(s, r.snapshot);
                });
                if (ok)
                    reply(std::move(r));
            } else if (id == SnapshotId) {
                EngineSnapshot v;
                ok = decode(b, [&](QDataStream &s) { read_snapshot(s, v); });
                if (ok)
                    publish(std::move(v));
            } else if (id == 14) {
                enabled = false;
                retry.stop();
                connection(State::Failed, QString::fromUtf8(b));
                socket->abort();
                return;
            } else if (id == TypedViewsId) {
                ViewFrame v;
                ok = read_typed_views(b, v);
                // Status snapshots may arrive before the polling mailbox's
                // unavailable snapshot. Stop fences data, but must deliver its clear.
                const bool clear_only=!v.views.isEmpty() && std::all_of(v.views.cbegin(),v.views.cend(),[](const auto &frame){return !frame.available;});
                if (ok && (subscriptions & EngineCommand::Views) && (subscriptions & EngineCommand::TypedViews) &&
                    (cache.statuses.value("Analyzer").state != State::Stopped || clear_only) &&
                    v.epoch == cache.epoch && v.graph_revision == cache.graph_revision && view_handler)
                    view_handler(std::move(v));
            } else if (id == ViewId) {
                ViewFrame v;
                ok = decode(b, [&](QDataStream &s) { read_view(s, v); });
                if (ok && (subscriptions & EngineCommand::Views) &&
                    v.epoch == cache.epoch && v.graph_revision == cache.graph_revision &&
                    v.spectrum.generation == cache.analyzer_generation &&
                    cache.statuses.value("Analyzer").state == State::Running) {
                    // An already-buffered packet can carry the previous mask.
                    // Filter before either caching its history or delivering it.
                    select_view_fields(v,subscriptions);
                    if (v.spectrum.discontinuity)
                        cache.history.reset();
                    if (v.history && !compatible_history(v.history, v.graph_revision, v.spectrum.generation))
                        v.history.reset();
                    if (v.history)
                        cache.history = v.history;
                    else if ((subscriptions & EngineCommand::History) &&
                             compatible_history(cache.history, v.graph_revision, v.spectrum.generation))
                        v.history = cache.history;
                    if (view_handler)
                        view_handler(std::move(v));
                }
            } else if (id == AudioId) {
                quint64 ep = 0;
                AudioFrame v;
                ok = decode(b, [&](QDataStream &s) {
                    s >> ep;
                    read_audio(s, v);
                });
                if (ok && (subscriptions & EngineCommand::Audio) &&
                    ep == cache.epoch && v.generation == cache.capture_generation &&
                    cache.statuses.value("Capture").state == State::Running && audio_handler)
                    audio_handler(std::move(v));
            } else
                ok = false;
            if (!ok) {
                if (b.size() >= 2 && (quint8(b[0]) != 1 || quint8(b[1]) != 0)) {
                    enabled = false;
                    retry.stop();
                    connection(State::Failed, "Unsupported engine protocol version");
                } else
                    connection(State::Recovering, "Malformed engine packet");
                socket->abort();
            }
        });
        enabled = true;
        connect();
    }
    void connect() {
        if (socket && enabled && socket->state() == QAbstractSocket::UnconnectedState) {
            wire->reset_assembly();
            connection(State::Starting);
            socket->connectToHost(host, port);
        }
    }
    ~Impl() {
        port_handler={};if(ports)ports->cancel();if(engine && port_token)engine->unsubscribe_ports(port_token);
        snapshot_handler = {};
        view_handler = {};
        audio_handler = {};
        reply_handler = {};
        enabled = false;
        retry.stop();
        if (socket)
            socket->abort();
        network.reset();
        if (engine) {
            views->cancel();
            audios->cancel();
            statuses->cancel();
            engine->set_status_handler({});
            engine->set_audio_handler({});
            engine->set_spectrum_handler({});
            commands.quit();
            commands.wait();
            engine.reset();
        }
    }
};
EngineClient::EngineClient(QObject *p) : EngineClient(AudioSourceFactory{}, p) {}
EngineClient::EngineClient(AudioSourceFactory source_factory, QObject *p)
    : QObject(p), d(std::make_unique<Impl>(this)) {
    d->local(std::move(source_factory));
}
EngineClient::EngineClient(QString h, quint16 p, QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {
    d->host = std::move(h);
    d->port = p;
    d->remote();
}
EngineClient::~EngineClient() = default;
quint64 EngineClient::submit(EngineCommand c) {
    if (c.kind == EngineCommand::Subscribe) {
        constexpr auto view_flags=EngineCommand::Views|EngineCommand::TypedViews|EngineCommand::History|
                                  EngineCommand::Previews|EngineCommand::Diagnostics;
        const auto changed=d->subscriptions ^ c.subscriptions;
        if (changed & view_flags) ++d->view_subscription_generation;
        if (changed & EngineCommand::Audio) ++d->audio_subscription_generation;
        d->subscriptions = c.subscriptions;
    }
    const auto id = ++d->next;
    d->pending[id] = QDateTime::currentMSecsSinceEpoch();
    if (d->pending.size() > 128 || (d->engine && d->outstanding.load() >= 128)) {
        d->pending.remove(id);
        QTimer::singleShot(0, this, [this, id] {
            if (d->reply_handler)
                d->reply_handler({id, false, "Command queue full", d->cache});
        });
        return id;
    }
    if (d->engine) {
        ++d->outstanding;
        QMetaObject::invokeMethod(
            d->worker,
            [this, id, c = std::move(c)] {
                if (c.kind == EngineCommand::Subscribe)
                    d->engine->set_view_subscription(c.subscriptions);
                auto r = execute(*d->engine, c, id, d->ep, d->rev);
                --d->outstanding;
                QMetaObject::invokeMethod(
                    this, [this, r = std::move(r)]() mutable { d->reply(std::move(r)); }, Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    } else if (d->socket->state() == QAbstractSocket::ConnectedState) {
        if(c.kind==EngineCommand::ApplyGraph)d->presentation_graph=c.graph;
        auto b = encode([&](QDataStream &s) {
            s << id << quint8(c.kind) << c.expected_revision << qint32(c.value) << c.subscriptions
              << c.interface_address.toString() << c.destination.toString() << c.port
              << QJsonDocument(desktop::processing_graph_document(c.graph)).toJson(QJsonDocument::Compact);
        });
        d->wire->send(CommandId, b);
    } else
        QTimer::singleShot(0, this, [this, id] { d->reply({id, false, "Not connected", d->cache}); });
    return id;
}
void EngineClient::set_snapshot_handler(std::function<void(EngineSnapshot)> h) {
    d->snapshot_handler = std::move(h);
}
void EngineClient::set_view_handler(std::function<void(ViewFrame)> h) {
    d->view_handler = std::move(h);
}
void EngineClient::set_audio_handler(std::function<void(AudioFrame)> h) {
    d->audio_handler = std::move(h);
}
void EngineClient::set_reply_handler(std::function<void(EngineReply)> h) {
    d->reply_handler = std::move(h);
}
EnginePort::PortDeliveryMode EngineClient::port_delivery_mode() const noexcept {return d->port_mode;}
bool EngineClient::subscribe_port_frames(std::vector<EndpointIdentity> endpoints,QString *error) {return d->request_ports(std::move(endpoints),error);}
void EngineClient::set_port_frame_handler(std::function<void(DeliveredPortFrame)> h) {d->port_handler=std::move(h);}
EngineSnapshot EngineClient::snapshot() const {
    return d->cache;
}
AudioPipeline *EngineClient::local_engine() const {
    return d->engine.get();
}
bool EngineClient::configure_network(QJsonObject config, QString *error) {
    d->network_warning.clear();
    if (!d->engine) {
        if (error)
            *error = "Remote client cannot configure local transports";
        return false;
    }
    // This composition owns an executor, but no EngineServer. Keep the user's
    // document intact and advertise only endpoints actually hosted here.
    auto server = config.value("engine_server").toObject();
    if (server.value("enabled").toBool(false))
        d->network_warning = "The local executor runs inside the GUI process and has no remote control endpoint. "
                             "Launch pipeline_cli --server with your configuration to provide one.";
    server["enabled"] = false;
    server["allow_mutations"] = false;
    server["port"] = 0;
    config["engine_server"] = server;
    d->network.reset();
    d->network = std::make_unique<NetworkRuntime>(*d->engine, std::move(config));
    QString message;
    bool accepted = d->network->refresh(message);
    if (error)
        *error = message;
    return accepted;
}
QString EngineClient::network_warning() const {
    return d->network_warning;
}
void EngineClient::connect_remote() {
    d->enabled = true;
    d->connect();
}
void EngineClient::disconnect_remote() {
    d->enabled = false;
    d->retry.stop();
    if (d->socket)
        d->socket->abort();
}
class DescriptorServer final : public QTcpServer {
  public:
    std::function<void(qintptr)> accept;

  protected:
    void incomingConnection(qintptr descriptor) override {
        if (accept)
            accept(descriptor);
    }
};
class EngineServer::Impl {
  public:
    EngineServer *owner;
    AudioPipeline &engine;
    DescriptorServer server;
    bool writable = false;
    quint64 ep = epoch(), rev = 0;
    quint64 canonical_sessions=epoch() & 0x7fffffffffffffffULL;
    QMap<TcpBDSPSocket *, std::shared_ptr<Wire>> clients;
    std::shared_ptr<LatestDelivery<ViewFrame>> views;
    std::shared_ptr<LatestDelivery<AudioFrame>> audios;
    std::shared_ptr<LatestDelivery<bool>> statuses;
    std::shared_ptr<PortMailbox> ports;
    uint64_t port_token=0;
    bool selected(const Wire &wire,const EndpointIdentity &id) const {
        const auto &selection=wire.port_subscription.endpoints;
        return selection.empty() || std::find(selection.begin(),selection.end(),id)!=selection.end();
    }
    bool send_ports(const std::shared_ptr<Wire> &wire,std::vector<DeliveredPortFrame> values,bool snapshot) {
        port_delivery::Frames frames;frames.snapshot=snapshot;frames.nonce=wire->port_nonce;frames.session_epoch=wire->port_session;
        frames.sequence=wire->port_sequence+1;frames.delivery={engine.graph_revision(),engine.epoch()};
        frames.graph_id=engine.graph().value("graph_id").toString("main").toStdString();
        for(auto &value:values)if(value.delivery==frames.delivery && selected(*wire,value.endpoint))frames.entries.push_back(std::move(value));
        if(snapshot)for(const auto &endpoint:wire->port_subscription.endpoints) {
            if(std::any_of(frames.entries.begin(),frames.entries.end(),[&](const auto &value){return value.endpoint==endpoint;}))continue;
            const auto marker=std::find_if(wire->port_markers.begin(),wire->port_markers.end(),[&](const auto &value){return value.endpoint==endpoint;});
            if(marker==wire->port_markers.end())return false;
            auto value=*marker;value.delivery=frames.delivery;frames.entries.push_back(std::move(value));
        }
        if(!snapshot && frames.entries.empty())return true;
        std::vector<uint8_t> bytes(wire->port_subscription.limits.max_message_bytes);
        const auto result=port_delivery::encode(frames,bytes,wire->port_subscription.limits);
        if(!result){wire->send(14,"Canonical port snapshot exceeds negotiated limits");wire->socket->disconnectFromHost();return false;}
        if(!wire->port_subscription.endpoints.empty())for(const auto &entry:frames.entries) {
            DeliveredPortFrame marker;marker.endpoint=entry.endpoint;marker.delivery=entry.delivery;marker.kind=entry.kind;marker.contract=entry.contract;marker.contract_known=entry.contract_known;
            marker.frame.type=entry.frame.type;marker.frame.available=false;marker.frame.sample_rate=entry.frame.sample_rate;marker.frame.fft_size=entry.frame.fft_size;marker.frame.bands=entry.frame.bands;marker.frame.history_capacity=entry.frame.history_capacity;
            marker.frame.epoch=entry.frame.epoch;marker.frame.producer_epoch=entry.frame.producer_epoch;marker.frame.generation=entry.frame.generation;marker.frame.graph_revision=entry.frame.graph_revision;marker.frame.stream_id=entry.frame.stream_id;
            auto previous=std::find_if(wire->port_markers.begin(),wire->port_markers.end(),[&](const auto &value){return value.endpoint==entry.endpoint;});
            if(previous==wire->port_markers.end())wire->port_markers.push_back(std::move(marker));else *previous=std::move(marker);
        }
        if(!wire->send(port_delivery::FramesId,QByteArray(reinterpret_cast<const char*>(bytes.data()),qsizetype(result.written_bytes))))return false;
        wire->port_sequence=frames.sequence;wire->port_revision=frames.delivery.graph_revision;wire->port_epoch=frames.delivery.epoch;
        return true;
    }
    void refresh_ports() {
        std::vector<EndpointIdentity> requested;
        bool any=false,all=false;
        for(auto wire:clients.values())if(wire->canonical) {
            any=true;all|=wire->port_subscription.endpoints.empty();
            for(const auto &endpoint:wire->port_subscription.endpoints)if(std::find(requested.begin(),requested.end(),endpoint)==requested.end())requested.push_back(endpoint);
        }
        if(port_token){engine.unsubscribe_ports(port_token);port_token=0;}
        if(!any)return;
        const auto available=engine.port_endpoints();
        if(!all)requested.erase(std::remove_if(requested.begin(),requested.end(),[&](const auto &endpoint){return std::find(available.begin(),available.end(),endpoint)==available.end();}),requested.end());
        if(!all && requested.empty())return;
        port_token=engine.subscribe_ports([mailbox=ports](DeliveredPortFrame value){mailbox->publish(std::move(value));},all?std::vector<EndpointIdentity>{}:requested);
    }
    void subscribe_ports(const std::shared_ptr<Wire> &wire,const QByteArray &bytes) {
        port_delivery::Subscribe request;
        const auto decoded=port_delivery::decode({reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size())},request);
        if(!decoded || request.nonce<=wire->port_last_nonce){wire->send(14,"Invalid or replayed canonical port subscription");wire->socket->disconnectFromHost();return;}
        // The replay fence advances on every syntactically valid request, including refusal.
        wire->port_last_nonce=request.nonce;
        port_delivery::Ack ack;ack.nonce=request.nonce;ack.session_epoch=wire->port_session;ack.graph_revision=engine.graph_revision();
        ack.graph_id=engine.graph().value("graph_id").toString("main").toStdString();ack.limits=request.limits;ack.type_mask=request.type_mask;
        ack.limits.max_message_bytes=std::min<uint32_t>(ack.limits.max_message_bytes,MaxMessage-2048);
        auto frames=engine.latest_port_frames(request.endpoints);
        for(const auto &endpoint:request.endpoints)if(std::none_of(frames.begin(),frames.end(),[&](const auto &f){return f.endpoint==endpoint;}))ack.status=port_delivery::Status::InvalidEndpoint;
        if(frames.size()>ack.limits.max_endpoints)ack.status=port_delivery::Status::LimitExceeded;
        for(const auto &frame:frames)if(!(ack.type_mask&(uint16_t(1)<<uint8_t(frame.frame.type))))ack.status=port_delivery::Status::UnsupportedType;
        port_delivery::Frames probe;probe.snapshot=true;probe.nonce=request.nonce;probe.session_epoch=wire->port_session;probe.sequence=1;
        probe.delivery={engine.graph_revision(),engine.epoch()};probe.graph_id=ack.graph_id;probe.entries=frames;
        if(ack.status==port_delivery::Status::Accepted) {
            std::vector<uint8_t> storage(ack.limits.max_message_bytes);
            const auto encoded=port_delivery::encode(probe,storage,ack.limits);
            const auto bound=encoded?stream::fragmented_message_bound(encoded.written_bytes,23,true):stream::InvalidByteBound;
            const auto ack_bound=stream::framed_packet_bound(40+ack.graph_id.size());
            if(!encoded || bound>stream::InvalidByteBound-ack_bound || wire->socket->bytesToWrite()<0 ||
               !stream::peer_budget_fits(size_t(wire->socket->bytesToWrite()),wire->socket->pending_packet_bytes(),wire->receiving?wire->total:0,bound+ack_bound))ack.status=port_delivery::Status::LimitExceeded;
        }
        if(ack.status!=port_delivery::Status::Accepted)ack.error="Requested endpoints, types or complete snapshot exceed server capabilities";
        std::array<uint8_t,1024> storage{};const auto result=port_delivery::encode(ack,storage);
        if(!result){wire->socket->abort();return;}
        if(!wire->send(port_delivery::AckId,QByteArray(reinterpret_cast<const char*>(storage.data()),qsizetype(result.written_bytes))))return;
        if(ack.status!=port_delivery::Status::Accepted)return;
        wire->port_markers.clear();
        wire->port_nonce=request.nonce;wire->port_subscription=request;wire->port_subscription.limits=ack.limits;wire->canonical=true;wire->port_sequence=0;
        send_ports(wire,std::move(frames),true);refresh_ports();
    }
    Impl(EngineServer *o, AudioPipeline &e) : owner(o), engine(e) {
        ports=std::make_shared<PortMailbox>(o,[this](std::vector<DeliveredPortFrame> frames,bool overflow) {
            for(auto wire:clients.values())if(wire->canonical) {
                if(overflow){wire->send(14,"Canonical delivery queue exceeded 2 MiB");wire->socket->disconnectFromHost();continue;}
                if(wire->port_revision!=engine.graph_revision() || wire->port_epoch!=engine.epoch()) {
                    send_ports(wire,frames,false);
                    send_ports(wire,engine.latest_port_frames(wire->port_subscription.endpoints),true);
                } else send_ports(wire,frames,false);
            }
        });ports->start();
        auto fragment_expiry=new QTimer(o);fragment_expiry->setInterval(100);
        QObject::connect(fragment_expiry,&QTimer::timeout,o,[this]{for(auto wire:clients.values())if(wire->receiving && wire->assembly_time.elapsed()>=5000)wire->socket->abort();});fragment_expiry->start();
        views = std::make_shared<LatestDelivery<ViewFrame>>(o, [this](ViewFrame v) {
            if (v.epoch != engine.epoch() || v.graph_revision != engine.graph_revision())
                return;
            if(std::any_of(clients.cbegin(),clients.cend(),[](auto wire){return (wire->subscriptions&EngineCommand::TypedViews)!=0;}))
                v.views=engine.latest_views();
            v.epoch = ep;
            QMap<quint64, QByteArray> payloads;
            for (auto wire : clients.values())
                if (wire->subscriptions & EngineCommand::Views) {
                    const auto mask = wire->subscriptions;
                    if (mask & EngineCommand::TypedViews) {
                        auto bytes = typed_views(v);
                        if (!bytes.isEmpty())
                            wire->send(TypedViewsId, bytes);
                        else {
                            wire->send(14, "Invalid or oversized typed view snapshot (2 MiB limit)");
                            wire->socket->disconnectFromHost();
                        }
                        continue;
                    }
                    const bool fresh = v.history && wire->last_history != v.history;
                    const auto key = quint64(mask) | (quint64(fresh) << 32);
                    if (!payloads.contains(key)) {
                        auto selected = v;
                        if (!fresh)
                            selected.history.reset();
                        if (!(mask & EngineCommand::History))
                            selected.history.reset();
                        if (!(mask & EngineCommand::Previews))
                            selected.previews.clear();
                        if (!(mask & EngineCommand::Diagnostics)) {
                            selected.window_signal.clear();
                            selected.window.clear();
                            selected.windowed_signal.clear();
                        }
                        payloads[key] = encode([&](QDataStream &s) { view(s, selected); });
                    }
                    wire->send(ViewId, payloads[key]);
                    if (mask & EngineCommand::History)
                        wire->last_history = v.history;
                }
        });
        audios = std::make_shared<LatestDelivery<AudioFrame>>(o, [this](AudioFrame v) {
            if (v.generation != engine.capture_generation() || engine.current_status("Capture").state != State::Running)
                return;
            auto b = encode([&](QDataStream &s) {
                s << ep;
                audio(s, v);
            });
            for (auto wire : clients.values())
                if (wire->subscriptions & EngineCommand::Audio)
                    wire->send(AudioId, b);
        });
        views->start_polling();
        audios->start_polling();
        engine.set_spectrum_handler([delivery = views](ViewFrame v) { delivery->publish(std::move(v)); });
        engine.set_audio_handler([delivery = audios](AudioFrame v) { delivery->publish(std::move(v)); });
        statuses = std::make_shared<LatestDelivery<bool>>(owner, [this](bool) { broadcast(); });
        engine.set_status_handler([delivery = statuses](QString, ComponentStatus) { delivery->publish(true); });
        server.accept = [this](qintptr descriptor) {
            auto *s = new TcpBDSPSocket(owner);
            if (!s->setSocketDescriptor(descriptor)) {
                delete s;
                return;
            }
            if (clients.size() >= 8) {
                s->abort();
                s->deleteLater();
                return;
            }
            if(canonical_sessions==UINT64_MAX){s->abort();s->deleteLater();return;}
            auto w = std::make_shared<Wire>(Wire{s});w->port_session=++canonical_sessions;
            clients[s] = w;
            update_subscriptions();
            s->setReadBufferSize(MaxMessage);
            QObject::connect(s, &QTcpSocket::disconnected, owner, [this, s] {
                clients.remove(s);
                refresh_ports();
                update_subscriptions();
                s->deleteLater();
            });
            QObject::connect(s, &TcpBDSPSocket::on_bdsp_packet_parsing_error, owner, [s](auto, auto *) { s->abort(); });
            QObject::connect(s, &TcpBDSPSocket::packetReceived, owner, [this, w](quint8 id, QByteArray b) {
                if (!w->receive(id, b))
                    return;
                if(id==port_delivery::SubscribeId){subscribe_ports(w,b);return;}
                if (id != CommandId) {
                    w->socket->abort();
                    return;
                }
                EngineCommand c;
                quint64 request = 0;
                quint8 kind = 0;
                QString address, destination;
                QByteArray graph;
                qint32 value = 0;
                if (!decode(b,
                            [&](QDataStream &s) {
                                s >> request >> kind >> c.expected_revision >> value >> c.subscriptions;
                                read_string(s, address, 256);
                                read_string(s, destination, 256);
                                s >> c.port;
                                read_blob(s, graph);
                            }) ||
                    kind > EngineCommand::Subscribe) {
                    if (b.size() >= 2 && (quint8(b[0]) != 1 || quint8(b[1]) != 0)) {
                        w->send(14, "Unsupported engine protocol version");
                        w->socket->disconnectFromHost();
                        return;
                    }
                    w->socket->abort();
                    return;
                }
                c.kind = static_cast<EngineCommand::Kind>(kind);
                c.value = value;
                c.interface_address = QHostAddress(address);
                c.destination = QHostAddress(destination);
                c.graph = QJsonDocument::fromJson(graph).object();
                EngineReply r;
                if (!writable && c.kind != EngineCommand::Snapshot && c.kind != EngineCommand::Subscribe)
                    r = {request, false, "Server is read-only", collect(engine, ep, rev)};
                else {
                    if (c.kind == EngineCommand::Subscribe) {
                        w->subscriptions = c.subscriptions;
                        update_subscriptions();
                    }
                    r = execute(engine, c, request, ep, rev);
                }
                auto payload = encode([&](QDataStream &s) {
                    s << r.request_id << r.accepted << r.error;
                    snapshot_codec(s, r.snapshot);
                });
                w->send(ReplyId, payload);
                if (r.accepted && c.kind != EngineCommand::Snapshot && c.kind != EngineCommand::Subscribe)
                    broadcast();
            });
        };
    }
    void update_subscriptions() {
        quint32 mask = 0;
        for (auto w : clients.values())
            mask |= w->subscriptions;
        engine.set_view_subscription(mask);
    }
    void broadcast() {
        auto v = collect(engine, ep, rev);
        auto b = encode([&](QDataStream &s) { snapshot_codec(s, v); });
        for (auto w : clients.values())
            w->send(SnapshotId, b);
    }
    ~Impl() {
        ports->cancel();if(port_token)engine.unsubscribe_ports(port_token);
        views->cancel();
        audios->cancel();
        statuses->cancel();
        engine.set_status_handler({});
        engine.set_audio_handler({});
        engine.set_spectrum_handler({});
        for (auto s : clients.keys()) {
            s->disconnect(owner);
            s->abort();
        }
        clients.clear();
    }
};
EngineServer::EngineServer(AudioPipeline &e, QObject *p) : QObject(p), d(std::make_unique<Impl>(this, e)) {}
EngineServer::~EngineServer() = default;
bool EngineServer::listen(const QHostAddress &a, quint16 p, bool writable) {
    d->writable = writable;
    return d->server.listen(a, p);
}
quint16 EngineServer::port() const {
    return d->server.serverPort();
}
QString EngineServer::error() const {
    return d->server.errorString();
}
} // namespace pipeline
