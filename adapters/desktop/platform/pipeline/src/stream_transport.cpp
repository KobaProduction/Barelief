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
QByteArray request(QString id, stream::Type type) {
    auto b = id.toUtf8();
    b.prepend(char(type));
    b.prepend(char(2));
    b.prepend(char(0));
    return b;
}
bool parse(const QByteArray &b, QString &id, stream::Type &type) {
    if (b.size() < 4 || b.size() > 259 || b[0] != 0 || b[1] != 2 || uint8_t(b[2]) > 6)
        return false;
    type = stream::Type(uint8_t(b[2]));
    id = QString::fromUtf8(b.mid(3));
    return !id.isEmpty() && id.toUtf8() == b.mid(3);
}
} // namespace
class NetworkPublisher::Impl {
  public:
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
                    s->send_packet(Error, "Unsupported stream/version/type");
                    s->disconnectFromHost();
                    return;
                }
                peers[s] = {id, type, true};
                s->send_packet(Ack, b);
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
    d->streams[id] = t;
}
uint64_t NetworkPublisher::dropped() const {
    return d->drops;
}
bool NetworkPublisher::publish(const QString &id, const stream::Frame &f) {
    if (!stream::valid(f) || !d->streams.contains(id) || d->streams.value(id) != f.type)
        return false;
    auto recipients = d->peers.keys();
    for (auto s : recipients) {
        if (!d->peers.contains(s))
            continue;
        auto &peer = d->peers[s];
        if (peer.id != id)
            continue;
        if (s->bytesToWrite() + qsizetype(stream::payload_size(f) + 256) > Queue) {
            peer.gap = true;
            ++d->drops;
            if (!f.available)
                s->abort(); // A control marker must not disappear behind stale data.
            continue;
        }
        auto x = f;
        x.epoch = d->epoch;
        x.discontinuity |= peer.gap;
        auto bytes = stream::encode(x);
        peer.gap = false;
        std::array<std::uint8_t, bdsp_protocol::StreamFragmentBytes + 8> fragment;
        for (size_t off = 0; off < bytes.size(); off += Chunk) {
            const auto encoded = bdsp_protocol::encode_fragment(bytes, off, fragment);
            if (!encoded || !s->send_packet(Data, QByteArray(reinterpret_cast<const char *>(fragment.data()),
                                                            qsizetype(encoded.written_bytes)))) {
                if (d->peers.contains(s))
                    d->peers[s].gap = true;
                ++d->drops;
                break;
            }
        }
    }
    return true;
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
    quint16 port = 0;
    stream::Type type;
    bool enabled = false, ready = false, gap = true;
    int attempt = 0;
    std::vector<std::uint8_t> assembly_storage = std::vector<std::uint8_t>(bdsp_protocol::MaxFrameBytes);
    bdsp_protocol::FrameAssembler assembler{assembly_storage};
    uint64_t epoch = 0, generation = 0, sequence = 0;
    std::function<void(stream::Frame)> frame;
    std::function<void(QString)> status;
    void report(QString s) {
        if (status)
            status(std::move(s));
    }
    void connect() {
        if (!enabled)
            return;
        ready = false;
        assembler.reset();
        timeout.start(5000);
        if(channel) {
            const auto opened=serial?serial->open(serial_endpoint):signal_contracts::Status::ok;
            if(opened!=signal_contracts::Status::ok) {
                report(serial->transport().error_string());
                recover();
            } else if(channel->send_packet(Subscribe,request(id,type))!=signal_contracts::Status::ok) {
                abort();
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
                         [this] { socket.send_packet(Subscribe, request(id, type)); });
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
                f.type != type) {
                report(QString::fromStdString(error));
                abort();
                return;
            }
            if (!f.available) {
                gap = true;
                if (frame)
                    frame(std::move(f));
                return;
            }
            if (f.epoch == epoch && f.generation == generation && f.sequence <= sequence && !f.discontinuity && !gap)
                return;
            f.discontinuity |= gap || f.epoch != epoch || f.generation != generation || f.sequence != sequence + 1;
            gap = false;
            epoch = f.epoch;
            generation = f.generation;
            sequence = f.sequence;
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
    stop();
    d->serial.reset();
    d->channel=nullptr;
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
    d->serial_session(std::move(endpoint));
    d->id=std::move(id);d->type=type;d->enabled=true;d->attempt=0;
    d->connect();
}
void NetworkSubscriber::start_channel(desktop::QtPacketChannel &channel,QString id,stream::Type type) {
    stop();d->serial.reset();
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
    d->report("Stopped");
}
void NetworkSubscriber::set_frame_handler(std::function<void(stream::Frame)> h) {
    d->frame = std::move(h);
}
void NetworkSubscriber::set_status_handler(std::function<void(QString)> h) {
    d->status = std::move(h);
}
} // namespace pipeline
