#include "pipeline/stream_transport.h"
#include "core/network/tcp_packet_socket.h"
#include <bdsp_protocol/stream_fragments.h>
#include <array>
#include <vector>
#include <QTcpServer>
#include <QTimer>
#include <QRandomGenerator>
#include <QPointer>
#include <map>
namespace pipeline {
namespace {
constexpr quint8 Subscribe = 11, Ack = 12, Data = 13, Error = 14;
constexpr qsizetype Chunk = bdsp_protocol::StreamFragmentBytes, Queue = 2 * 1024 * 1024;
bool send_bounded_packet(TcpBDSPSocket &socket,quint8 kind,const QByteArray &payload) {
    if(socket.bytesToWrite()<0 || !stream::peer_budget_fits(size_t(socket.bytesToWrite()),socket.pending_packet_bytes(),0,stream::framed_packet_bound(size_t(payload.size()))) || !socket.send_packet(kind,payload)){socket.abort();return false;}return true;
}
uint64_t scoped_producer(uint64_t publisher, uint64_t producer) {
    // Local producer epochs can repeat in a new process. Scope inherited identity
    // by this publisher incarnation, including at stable intermediate relays.
    auto value=publisher ^ (producer + 0x9e3779b97f4a7c15ULL);
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27))*0x94d049bb133111ebULL;
    value^=value>>31;
    return value?value:1;
}
QByteArray request(QString id, stream::Type type) {
    auto b = id.toUtf8();
    b.prepend(char(type));
    b.prepend(char(int(type)>=7?3:2));
    b.prepend(char(0));
    return b;
}
bool parse(const QByteArray &b, QString &id, stream::Type &type) {
    if (b.size() < 4 || b.size() > 259 || b[0] != 0 || !signal_contracts::frame_descriptor(stream::Type(uint8_t(b[2]))) ||
        b[1] != (uint8_t(b[2])>=7?3:2))
        return false;
    type = stream::Type(uint8_t(b[2]));
    id = QString::fromUtf8(b.mid(3));
    return !id.isEmpty() && id.toUtf8() == b.mid(3);
}
} // namespace
class NetworkPublisher::Impl {
  public:
    std::map<QString,uint64_t> last_producers;
    struct Server : QTcpServer {
        std::function<void(qintptr)> accept;
        void incomingConnection(qintptr fd) override {
            accept(fd);
        }
    };
    Server server;
    struct Peer {
        QString id;
        stream::Type type = stream::Type::PCM;
        bool gap = true;
    };
    QMap<TcpBDSPSocket *, Peer> peers;
    QMap<QString, stream::Type> streams;
    uint64_t drops = 0, epoch = QRandomGenerator::global()->generate64();
    Impl(NetworkPublisher *o) {
        server.accept = [this, o](qintptr fd) {
            auto s = new TcpBDSPSocket(o);
            if (!s->setSocketDescriptor(fd)) {
                delete s;
                return;
            }
            if (peers.size() >= 16) {
                s->abort();
                s->deleteLater();
                return;
            }
            s->setReadBufferSize(stream::MaxFrameBytes);
            QObject::connect(s, &TcpBDSPSocket::on_bdsp_packet_parsing_error, o, [s] { s->abort(); });
            peers[s] = {};
            QObject::connect(s, &QTcpSocket::disconnected, o, [this, s] {
                peers.remove(s);
                s->deleteLater();
            });
            QObject::connect(s, &TcpBDSPSocket::packetReceived, o, [this, s](quint8 kind, QByteArray b) {
                QString id;
                stream::Type type;
                if (kind != Subscribe || !parse(b, id, type) || !streams.contains(id) || streams[id] != type) {
                    send_bounded_packet(*s,Error,"Unsupported stream/version/type");
                    s->disconnectFromHost();
                    return;
                }
                peers[s] = {id, type, true};
                send_bounded_packet(*s,Ack,b);
            });
            QTimer::singleShot(5000, s, [this, s] {
                if (peers.contains(s) && peers[s].id.isEmpty())
                    s->abort();
            });
        };
    }
};
NetworkPublisher::NetworkPublisher(QObject *p) : QObject(p), d(std::make_unique<Impl>(this)) {}
NetworkPublisher::~NetworkPublisher() {
    stop();
}
bool NetworkPublisher::listen(const QHostAddress &a, quint16 p) {
    d->epoch = QRandomGenerator::global()->generate64();
    for(auto &[id,producer]:d->last_producers)producer=0;
    return d->server.listen(a, p);
}
quint16 NetworkPublisher::port() const {
    return d->server.serverPort();
}
void NetworkPublisher::stop() {
    d->server.close();
    auto sockets = d->peers.keys();
    for (auto s : sockets)
        s->abort();
    d->peers.clear();
}
void NetworkPublisher::advertise(QString id, stream::Type t) {
    try_advertise(std::move(id),t);
}
bool NetworkPublisher::try_advertise(QString id,stream::Type t) {
    const auto bytes=id.toUtf8();
    if(bytes.isEmpty() || bytes.size()>256 || bytes.contains(char(0)) || !signal_contracts::frame_descriptor(t) ||
       (d->streams.contains(id)?d->streams.value(id)!=t:d->streams.size()>=128))return false;
    d->streams[id]=t;d->last_producers.try_emplace(id,0);return true;
}
StreamLimits NetworkPublisher::limits() const {return {};}
uint64_t NetworkPublisher::dropped() const {
    return d->drops;
}
uint64_t NetworkPublisher::incarnation() const {return d->epoch;}
bool NetworkPublisher::publish(const QString &id, const stream::Frame &f) {
    if (!stream::valid(f) || !d->streams.contains(id) || d->streams.value(id) != f.type)
        return false;
    const auto id_size=size_t(id.toUtf8().size());const auto without_id=stream::payload_size(f)-f.stream_id.size();
    if(id_size>limits().max_frame_bytes || without_id>limits().max_frame_bytes-id_size)return false;
    auto &last_producer=d->last_producers.at(id);
    const bool unknown_marker=!f.available && !f.producer_epoch && !f.generation;
    const auto producer=unknown_marker && last_producer?last_producer:
        scoped_producer(d->epoch,unknown_marker?0:(f.producer_epoch?f.producer_epoch:f.epoch));
    last_producer=producer;
    bool accepted=true;
    const auto serialized_size=without_id+id_size;
    const auto framed_bound=stream::fragmented_message_bound(serialized_size,8);
    auto recipients = d->peers.keys();
    for (auto s : recipients) {
        if (!d->peers.contains(s))
            continue;
        auto &peer = d->peers[s];
        if (peer.id != id)
            continue;
        if (s->bytesToWrite()<0 || !stream::peer_budget_fits(size_t(s->bytesToWrite()),s->pending_packet_bytes(),0,framed_bound)) {
            peer.gap = true;
            accepted=false;
            ++d->drops;
            if (!f.available)
                s->abort(); // A control marker must not disappear behind stale data.
            continue;
        }
        auto x = f;
        x.epoch = d->epoch;
        x.stream_id = id.toStdString();
        x.producer_epoch=producer;
        x.discontinuity |= peer.gap;
        auto bytes = stream::encode(x);
        bool sent=true;
        std::array<std::uint8_t, bdsp_protocol::StreamFragmentBytes + 8> fragment;
        for (size_t off = 0; off < bytes.size(); off += Chunk) {
            const auto encoded = bdsp_protocol::encode_fragment(bytes, off, fragment);
            if (!encoded || !s->send_packet(Data, QByteArray(reinterpret_cast<const char *>(fragment.data()),
                                                            qsizetype(encoded.written_bytes)))) {
                if (d->peers.contains(s))
                    d->peers[s].gap = true;
                ++d->drops;accepted=false;sent=false;s->abort();
                break;
            }
        }
        if(d->peers.contains(s) && s->state()==QAbstractSocket::ConnectedState)d->peers[s].gap=!sent;
    }
    return accepted;
}
class NetworkSubscriber::Impl {
  public:
    NetworkSubscriber *owner;
    TcpBDSPSocket socket;
    std::unique_ptr<desktop::QtSerialPacketSession> serial;
    desktop::SerialEndpoint serial_endpoint;
    QPointer<desktop::QtPacketChannel> channel;
    uint64_t binding=0;
    std::vector<QMetaObject::Connection> channel_connections;
    QTimer retry, timeout;
    QString host, id;
    std::string id_bytes;
    quint16 port = 0;
    stream::Type type;
    bool enabled = false, ready = false, gap = true;
    int attempt = 0;
    std::vector<std::uint8_t> assembly_storage = std::vector<std::uint8_t>(bdsp_protocol::MaxFrameBytes);
    bdsp_protocol::FrameAssembler assembler{assembly_storage};
    uint64_t expected_publisher_incarnation=0;
    uint64_t epoch = 0, producer_epoch = 0, generation = 0, sequence = 0;
    struct ProducerIdentity {
        uint64_t publisher=0,producer=0;
        bool operator==(const ProducerIdentity &) const = default;
    };
    std::array<ProducerIdentity,16> retired{};
    size_t retired_count=0;
    bool identity_known=false;
    std::function<void(stream::Frame)> frame;
    std::function<void(QString)> status;
    void report(QString s) {
        if (status)
            status(std::move(s));
    }
    void connect() {
        if (!enabled)
            return;
        id_bytes=id.toStdString();
        ready = false;
        assembler.reset();
        timeout.start(5000);
        if(channel) {
            const auto opened=serial?serial->open(serial_endpoint):signal_contracts::Status::ok;
            if(opened!=signal_contracts::Status::ok) {
                report(serial->transport().error_string());
                recover();
            } else {
                const auto payload=request(id,type);const auto queued=channel->device_pending_bytes();
                if(queued<0 || !stream::peer_budget_fits(size_t(queued),channel->pending_bytes(),0,stream::framed_packet_bound(size_t(payload.size()))) || channel->send_packet(Subscribe,payload)!=signal_contracts::Status::ok)abort();
            }
        } else socket.connectToHost(host, port);
    }
    void abort() {
        if(channel) {if(serial) serial->close();else channel->reset();recover();}else socket.abort();
    }
    void recover() {
        ready = false;
        gap = true;
        assembler.reset();
        timeout.stop();
        if (enabled && !retry.isActive()) {
            report("Recovering");
            retry.start(attempt == 0 ? 1000 : attempt == 1 ? 2000 : 5000);
            ++attempt;
        }
    }
    Impl(NetworkSubscriber *o) : owner(o), socket(o), retry(o), timeout(o) {
        socket.setReadBufferSize(stream::MaxFrameBytes);
        retry.setSingleShot(true);
        timeout.setSingleShot(true);
        QObject::connect(&socket, &TcpBDSPSocket::on_bdsp_packet_parsing_error, o, [this] { socket.abort(); });
        QObject::connect(&retry, &QTimer::timeout, o, [this] { connect(); });
        QObject::connect(&timeout, &QTimer::timeout, o, [this] {
            abort();
            recover();
        });
        QObject::connect(&socket, &QTcpSocket::connected, o,
                         [this] { send_bounded_packet(socket,Subscribe,request(id,type)); });
        QObject::connect(&socket, &QTcpSocket::disconnected, o, [this] { recover(); });
        QObject::connect(&socket, &QTcpSocket::errorOccurred, o, [this] { recover(); });
        QObject::connect(&socket, &TcpBDSPSocket::packetReceived, o,
                         [this](quint8 kind,QByteArray b){receive(kind,std::move(b));});
    }
    void receive(quint8 kind,QByteArray b) {
            if (!enabled)
                return;
            if (kind == Error) {
                enabled = false;
                retry.stop();
                timeout.stop();
                report(QString::fromUtf8(b));
                abort();
                return;
            }
            if (kind == Ack) {
                if (b != request(id, type)) {
                    abort();
                    return;
                }
                ready = true;
                attempt = 0;
                timeout.stop();
                report("Running");
                return;
            }
            if (kind != Data || !ready || b.size() < 9) {
                abort();
                return;
            }
            const auto *raw=reinterpret_cast<const uint8_t*>(b.constData());
            const size_t declared=size_t(raw[0])|(size_t(raw[1])<<8)|(size_t(raw[2])<<16)|(size_t(raw[3])<<24);
            const auto queued=channel?channel->device_pending_bytes():socket.bytesToWrite();
            const size_t pending=channel?channel->pending_bytes():socket.pending_packet_bytes();
            if(queued<0 || !stream::peer_budget_fits(size_t(queued),pending,declared,0)){abort();return;}
            std::span<const std::uint8_t> complete;
            const auto assembled = assembler.accept({reinterpret_cast<const std::uint8_t *>(b.constData()),
                                                      std::size_t(b.size())}, complete);
            if (assembled == signal_contracts::Status::would_block)
                return;
            if (assembled != signal_contracts::Status::ok) {
                abort();
                return;
            }
            stream::Frame f;
            std::string error;
            if (!stream::decode(complete, f, error) ||
                f.type != type || f.stream_id != id_bytes) {
                report(error.empty()?QStringLiteral("Stream type or id does not match subscription"):QString::fromStdString(error));
                abort();
                return;
            }
            // Registry identity authorizes the publisher; first DATA cannot establish or replace it.
            if(expected_publisher_incarnation && f.epoch!=expected_publisher_incarnation)return;
            auto producer=f.producer_epoch?f.producer_epoch:f.epoch;
            if(!f.available && !f.producer_epoch && !f.generation && identity_known && f.epoch==epoch)producer=producer_epoch;
            const ProducerIdentity incoming{f.epoch,producer},current{epoch,producer_epoch};
            const bool same_producer=identity_known && incoming==current;
            if(!same_producer && std::find(retired.begin(),retired.begin()+retired_count,incoming)!=retired.begin()+retired_count)return;
            if(same_producer && f.generation<generation && (f.available || f.generation))return;
            const bool same_identity=same_producer && (f.generation==generation || (!f.available && !f.generation));
            if(f.available && same_identity && f.sequence<=sequence)return;
            f.discontinuity |= gap || !same_identity || (f.available && f.sequence != sequence + 1);
            if(!same_producer && identity_known) {
                // Finite policy: never forget a retired producer; only explicit start may resynchronize.
                if(retired_count==retired.size()){enabled=false;retry.stop();timeout.stop();assembler.reset();report("Producer retirement exhausted; explicit resubscription required");abort();return;}
                retired[retired_count++]=current;
            }
            if(f.available || !same_identity)sequence=f.sequence;
            gap = !f.available;
            epoch = f.epoch;
            producer_epoch=producer;
            identity_known=true;
            if(f.available || !same_identity || f.generation)generation=f.generation;
            if (frame)
                frame(std::move(f));
    }
    void serial_session(desktop::SerialEndpoint endpoint) {
        serial_endpoint=std::move(endpoint);
        serial=std::make_unique<desktop::QtSerialPacketSession>();
        bind_channel(serial->channel());
        const auto current=binding;
        QObject::connect(&serial->transport().device(),&QSerialPort::errorOccurred,owner,
                         [this,current](QSerialPort::SerialPortError error) {
            if(current==binding && enabled && serial && error!=QSerialPort::NoError) {
                report(serial->transport().error_string());abort();
            }
        },Qt::QueuedConnection);
    }
    void bind_channel(desktop::QtPacketChannel &next) {
        channel=&next;
        const auto current=binding;
        channel_connections.push_back(QObject::connect(channel,&desktop::QtPacketChannel::packetReceived,owner,
                         [this,current](quint8 kind,QByteArray b) {
            if(current==binding) receive(kind,std::move(b));
        }));
        channel_connections.push_back(QObject::connect(channel,&desktop::QtPacketChannel::protocolError,owner,
                         [this,current]{if(current==binding) abort();}));
        channel_connections.push_back(QObject::connect(channel,&desktop::QtPacketChannel::parsingError,owner,
                         [this,current]{if(current==binding) abort();}));
        channel_connections.push_back(QObject::connect(channel,&QObject::destroyed,owner,[this,current] {
            if(current!=binding) return;
            enabled=false;ready=false;retry.stop();timeout.stop();assembler.reset();gap=true;
            report("Stopped");
        }));
    }
};
NetworkSubscriber::NetworkSubscriber(QObject *p) : QObject(p), d(std::make_unique<Impl>(this)) {}
NetworkSubscriber::~NetworkSubscriber() {
    stop();
}
void NetworkSubscriber::start(QString h, quint16 p, QString id, stream::Type t) {
    start(std::move(h),p,std::move(id),t,{});
}
void NetworkSubscriber::start(QString h, quint16 p, QString id, stream::Type t,StreamLimits limits) {
    start(std::move(h),p,std::move(id),t,limits,0);
}
void NetworkSubscriber::start(QString h, quint16 p, QString id, stream::Type t,StreamLimits limits,uint64_t publisher_incarnation) {
    stop();
    d->serial.reset();
    d->channel=nullptr;
    const auto bytes=id.toUtf8();
    if(limits.max_frame_bytes<113 || limits.max_frame_bytes>stream::MaxFrameBytes || !limits.max_stream_id_bytes ||
       limits.max_stream_id_bytes>256 || !limits.max_streams || limits.max_streams>128 || bytes.isEmpty() ||
       size_t(bytes.size())>limits.max_stream_id_bytes || bytes.contains(char(0)) || 112+size_t(bytes.size())>limits.max_frame_bytes ||
       !signal_contracts::frame_descriptor(t)){d->report("Unsupported stream limits/id/type");return;}
    d->assembly_storage.resize(limits.max_frame_bytes);
    d->assembler=bdsp_protocol::FrameAssembler{d->assembly_storage};
    d->socket.setReadBufferSize(qint64(limits.max_frame_bytes));
    d->expected_publisher_incarnation=publisher_incarnation;
    d->host = std::move(h);
    d->port = p;
    d->id = std::move(id);
    d->type = t;
    d->enabled = true;
    d->attempt = 0;
    d->connect();
}
void NetworkSubscriber::start_serial(desktop::SerialEndpoint endpoint,QString id,stream::Type type) {
    stop();
    d->assembly_storage.resize(stream::MaxFrameBytes);d->assembler=bdsp_protocol::FrameAssembler{d->assembly_storage};
    if(id.isEmpty() || id.toUtf8().size()>256 || id.contains(QChar(0)) || !signal_contracts::frame_descriptor(type)){d->report("Unsupported stream id/type");return;}
    d->serial_session(std::move(endpoint));
    d->id=std::move(id);d->type=type;d->enabled=true;d->attempt=0;
    d->connect();
}
void NetworkSubscriber::start_channel(desktop::QtPacketChannel &channel,QString id,stream::Type type) {
    stop();d->serial.reset();
    d->assembly_storage.resize(stream::MaxFrameBytes);d->assembler=bdsp_protocol::FrameAssembler{d->assembly_storage};
    if(id.isEmpty() || id.toUtf8().size()>256 || id.contains(QChar(0)) || !signal_contracts::frame_descriptor(type)){d->report("Unsupported stream id/type");return;}
    d->bind_channel(channel);
    d->id=std::move(id);d->type=type;d->enabled=true;d->attempt=0;d->connect();
}
void NetworkSubscriber::stop() {
    d->enabled = false;
    ++d->binding;
    d->retry.stop();
    d->timeout.stop();
    d->socket.abort();
    if(d->serial) d->serial->close();
    else if(d->channel) d->channel->reset();
    for(const auto &connection:d->channel_connections) QObject::disconnect(connection);
    d->channel_connections.clear();
    d->assembler.reset();
    d->ready = false;
    d->gap = true;
    d->epoch=d->producer_epoch=d->generation=d->sequence=0;
    d->identity_known=false;d->retired_count=0;
    d->expected_publisher_incarnation=0;
    d->report("Stopped");
}
void NetworkSubscriber::set_frame_handler(std::function<void(stream::Frame)> h) {
    d->frame = std::move(h);
}
void NetworkSubscriber::set_status_handler(std::function<void(QString)> h) {
    d->status = std::move(h);
}
} // namespace pipeline
