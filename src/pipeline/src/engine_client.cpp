#include "pipeline/engine_client.h"
#include "core/network/tcp_packet_socket.h"
#include "core/latest_delivery.h"
#include <QTcpServer>
#include <QTimer>
#include <QDateTime>
#include <QDataStream>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QPointer>
#include <algorithm>
namespace pipeline {
namespace {
constexpr quint8 CommandId = 5, ReplyId = 6, SnapshotId = 7, ViewId = 8, AudioId = 9, FragmentId = 10;
constexpr qsizetype MaxMessage = 2 * 1024 * 1024, FragmentSize = 60000, MaxQueued = 2 * 1024 * 1024;
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
    s >> v.sequence >> v.timestamp_ns >> rate >> v.generation >> v.discontinuity;
    v.sample_rate = rate;
    read_floats(s, v.left);
    read_floats(s, v.right);
}
void view(QDataStream &s, const ViewFrame &v) {
    const auto &f = v.spectrum;
    s << v.epoch << quint64(v.graph_revision) << quint64(v.ready_timestamp_ns) << v.surface << quint64(f.sequence)
      << qint64(f.timestamp_ns) << qint64(f.input_timestamp_ns) << quint64(f.audio_samples) << f.audio_peak
      << quint64(f.sample_rate) << quint64(f.generation) << f.discontinuity << quint64(f.dropped_windows);
    for (auto p : {&f.left, &f.right, &f.mel_left, &f.mel_right, &v.fft, &v.mel, &v.samples, &v.window, &v.post_window})
        floats(s, *p);
    s << v.frequency_step << quint32(v.previews.size());
    for (auto i = v.previews.begin(); i != v.previews.end(); ++i)
        s << qint32(i.key()) << quint64(i->values) << i->peak;
    history(s, compatible_history(v.history, v.graph_revision, f.generation) ? v.history : nullptr);
}
void read_view(QDataStream &s, ViewFrame &v) {
    auto &f = v.spectrum;
    quint64 samples = 0, rate = 0;
    s >> v.epoch >> v.graph_revision >> v.ready_timestamp_ns >> v.surface >> f.sequence >> f.timestamp_ns >>
        f.input_timestamp_ns >> samples >> f.audio_peak >> rate >> f.generation >> f.discontinuity >> f.dropped_windows;
    f.audio_samples = samples;
    f.sample_rate = rate;
    for (auto p : {&f.left, &f.right, &f.mel_left, &f.mel_right, &v.fft, &v.mel, &v.samples, &v.window, &v.post_window})
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
        v.statuses.value("Analyzer").state != State::Running) v.history.reset();
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
            case EngineCommand::StartAnalyzer:
                e.start_analyzer();
                break;
            case EngineCommand::StopAnalyzer:
                e.stop_analyzer();
                break;
            case EngineCommand::StartCapture:
                e.start_capture();
                break;
            case EngineCommand::StopCapture:
                e.stop_capture();
                break;
            case EngineCommand::StartWled:
                e.start_wled(c.interface_address, c.port, c.destination);
                break;
            case EngineCommand::StopWled:
                e.stop_wled();
                break;
            case EngineCommand::SetMerge:
                if (c.value < 0 || c.value > 2) {
                    r.accepted = false;
                    r.error = "Invalid merge";
                } else
                    e.set_merge(static_cast<SpectrumMerge>(c.value));
                break;
            case EngineCommand::SetDisplayChannel:
                if (c.value < 0 || c.value > 2) {
                    r.accepted = false;
                    r.error = "Invalid channel";
                } else
                    e.set_display_channel(c.value);
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
// One bounded assembly per socket. TCP preserves fragment order. A new message cannot interleave.
struct Wire {
    TcpBDSPSocket *socket;
    quint64 serial = 0, receiving = 0;
    quint8 kind = 0;
    quint32 total = 0;
    QByteArray partial;
    quint32 subscriptions = EngineCommand::Views | EngineCommand::History | EngineCommand::Previews;
    std::shared_ptr<const HistorySnapshot> last_history;
    void send(quint8 id, const QByteArray &b) {
        if (b.size() > MaxMessage || socket->bytesToWrite() + b.size() > MaxQueued) {
            socket->abort();
            return;
        }
        if (b.size() <= 65535) {
            socket->send_data(id, reinterpret_cast<uint8_t *>(const_cast<char *>(b.data())), b.size());
            return;
        }
        const auto message = ++serial;
        for (qsizetype off = 0; off < b.size(); off += FragmentSize) {
            auto fragment = encode([&](QDataStream &s) {
                s << id << message << quint32(b.size()) << quint32(off) << b.mid(off, FragmentSize);
            });
            socket->send_data(FragmentId, reinterpret_cast<uint8_t *>(fragment.data()), fragment.size());
        }
    }
    bool receive(quint8 &id, QByteArray &b) {
        if (id != FragmentId)
            return b.size() <= 65535;
        quint8 k = 0;
        quint64 seq = 0;
        quint32 n = 0, off = 0;
        QByteArray chunk;
        if (!decode(b,
                    [&](QDataStream &s) {
                        s >> k >> seq >> n >> off;
                        read_blob(s, chunk, FragmentSize);
                    }) ||
            k < CommandId || k > AudioId || n > MaxMessage || n <= 65535 || chunk.size() > FragmentSize ||
            off + chunk.size() > n) {
            socket->abort();
            return false;
        }
        if (off == 0) {
            receiving = seq;
            kind = k;
            total = n;
            partial.clear();
            partial.reserve(n);
        }
        if (seq != receiving || k != kind || n != total || off != quint32(partial.size())) {
            socket->abort();
            return false;
        }
        partial.append(chunk);
        if (partial.size() != total)
            return false;
        id = kind;
        b = std::move(partial);
        receiving = 0;
        return true;
    }
};
} // namespace
class EngineClient::Impl {
  public:
    EngineClient *owner;
    std::unique_ptr<AudioPipeline> engine;
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
    quint32 subscriptions = EngineCommand::Views | EngineCommand::History | EngineCommand::Previews;
    EngineSnapshot cache;
    QMap<quint64, qint64> pending;
    std::function<void(EngineSnapshot)> snapshot_handler;
    std::function<void(ViewFrame)> view_handler;
    std::function<void(AudioFrame)> audio_handler;
    std::function<void(EngineReply)> reply_handler;
    std::shared_ptr<LatestDelivery<ViewFrame>> views;
    std::shared_ptr<LatestDelivery<AudioFrame>> audios;
    std::shared_ptr<LatestDelivery<bool>> statuses;
    Impl(EngineClient *o) : owner(o) {
        retry.setSingleShot(true);
        QObject::connect(&retry, &QTimer::timeout, o, [this] { connect(); });
        auto *expiry = new QTimer(o);
        expiry->setInterval(100);
        QObject::connect(expiry, &QTimer::timeout, o, [this] {
            const auto now = QDateTime::currentMSecsSinceEpoch();
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
        // Validate against the original generation before monotonic state merging.
        if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation)) v.history.reset();
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
                if (i->revision > v.statuses.value(i.key()).revision) v.statuses[i.key()] = i.value();
            if (v.revision < cache.revision || v.graph_revision < cache.graph_revision) {
                v.graph = cache.graph; v.graph_revision = cache.graph_revision;
            }
            v.revision = std::max(v.revision, cache.revision);
            v.analyzer_generation = std::max(v.analyzer_generation, cache.analyzer_generation);
            v.capture_generation = std::max(v.capture_generation, cache.capture_generation);
            if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation)) v.history.reset();
            if (!v.history && compatible_history(cache.history, v.graph_revision, v.analyzer_generation) &&
                v.statuses.value("Analyzer").state == State::Running)
                v.history = cache.history;
        }
        if (!compatible_history(v.history, v.graph_revision, v.analyzer_generation) ||
            v.statuses.value("Analyzer").state != State::Running) v.history.reset();
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
    void local() {
        engine = std::make_unique<AudioPipeline>();
        worker = new QObject;
        worker->moveToThread(&commands);
        QObject::connect(&commands, &QThread::finished, worker, &QObject::deleteLater);
        commands.start();
        cache = collect(*engine, ep, rev);
        views = std::make_shared<LatestDelivery<ViewFrame>>(owner, [this](ViewFrame v) {
            v.epoch = ep;
            if (v.graph_revision != engine->graph_revision() ||
                v.spectrum.generation != engine->analyzer_generation() ||
                engine->current_status("Analyzer").state != State::Running)
                return;
            if (view_handler)
                view_handler(std::move(v));
        });
        audios = std::make_shared<LatestDelivery<AudioFrame>>(owner, [this](AudioFrame v) {
            if (v.generation != engine->capture_generation() ||
                engine->current_status("Capture").state != State::Running)
                return;
            if (audio_handler)
                audio_handler(std::move(v));
        });
        engine->set_spectrum_handler([delivery = views](ViewFrame v) { delivery->publish(std::move(v)); });
        engine->set_audio_handler([delivery = audios](AudioFrame v) { delivery->publish(std::move(v)); });
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
            if (id == ReplyId) {
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
            } else if (id == ViewId) {
                ViewFrame v;
                ok = decode(b, [&](QDataStream &s) { read_view(s, v); });
                if (ok && v.epoch == cache.epoch && v.graph_revision == cache.graph_revision &&
                    v.spectrum.generation == cache.analyzer_generation &&
                    cache.statuses.value("Analyzer").state == State::Running) {
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
                if (ok && ep == cache.epoch && v.generation == cache.capture_generation &&
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
            wire->partial.clear();
            connection(State::Starting);
            socket->connectToHost(host, port);
        }
    }
    ~Impl() {
        snapshot_handler = {};
        view_handler = {};
        audio_handler = {};
        reply_handler = {};
        enabled = false;
        retry.stop();
        if (socket)
            socket->abort();
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
EngineClient::EngineClient(QObject *p) : QObject(p), d(std::make_unique<Impl>(this)) {
    d->local();
}
EngineClient::EngineClient(QString h, quint16 p, QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {
    d->host = std::move(h);
    d->port = p;
    d->remote();
}
EngineClient::~EngineClient() = default;
quint64 EngineClient::submit(EngineCommand c) {
    if (c.kind == EngineCommand::Subscribe)
        d->subscriptions = c.subscriptions;
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
        auto b = encode([&](QDataStream &s) {
            s << id << quint8(c.kind) << c.expected_revision << qint32(c.value) << c.subscriptions
              << c.interface_address.toString() << c.destination.toString() << c.port
              << QJsonDocument(c.graph).toJson(QJsonDocument::Compact);
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
EngineSnapshot EngineClient::snapshot() const {
    return d->cache;
}
AudioPipeline *EngineClient::local_engine() const {
    return d->engine.get();
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
    QMap<TcpBDSPSocket *, std::shared_ptr<Wire>> clients;
    std::shared_ptr<LatestDelivery<ViewFrame>> views;
    std::shared_ptr<LatestDelivery<AudioFrame>> audios;
    std::shared_ptr<LatestDelivery<bool>> statuses;
    Impl(EngineServer *o, AudioPipeline &e) : owner(o), engine(e) {
        views = std::make_shared<LatestDelivery<ViewFrame>>(o, [this](ViewFrame v) {
            if (v.spectrum.generation != engine.analyzer_generation() || v.graph_revision != engine.graph_revision() ||
                engine.current_status("Analyzer").state != State::Running)
                return;
            v.epoch = ep;
            QMap<quint64, QByteArray> payloads;
            for (auto wire : clients.values())
                if (wire->subscriptions & EngineCommand::Views) {
                    const auto mask = wire->subscriptions;
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
                            selected.samples.clear();
                            selected.window.clear();
                            selected.post_window.clear();
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
            auto w = std::make_shared<Wire>(Wire{s});
            clients[s] = w;
            update_subscriptions();
            s->setReadBufferSize(MaxMessage);
            QObject::connect(s, &QTcpSocket::disconnected, owner, [this, s] {
                clients.remove(s);
                update_subscriptions();
                s->deleteLater();
            });
            QObject::connect(s, &TcpBDSPSocket::on_bdsp_packet_parsing_error, owner, [s](auto, auto *) { s->abort(); });
            QObject::connect(s, &TcpBDSPSocket::packetReceived, owner, [this, w](quint8 id, QByteArray b) {
                if (!w->receive(id, b))
                    return;
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
