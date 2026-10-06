#include "pipeline/stream_transport.h"
#include "core/network/tcp_packet_socket.h"
#include <QTcpServer>
#include <QTimer>
#include <QRandomGenerator>
#include <QPointer>
#include <map>
namespace pipeline {
namespace {
constexpr quint8 Subscribe = 11, Ack = 12, Data = 13, Error = 14;
constexpr qsizetype Chunk = 60000, Queue = 2 * 1024 * 1024;
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
void put_integer(QByteArray &b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.append(char(v >> (i * 8)));
}
uint32_t get_integer(const QByteArray &b, int p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= uint32_t(uint8_t(b[p + i])) << (i * 8);
    return v;
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
        for (size_t off = 0; off < bytes.size(); off += Chunk) {
            QByteArray b;
            put_integer(b, bytes.size());
            put_integer(b, off);
            b.append(reinterpret_cast<const char *>(bytes.data() + off), std::min<size_t>(Chunk, bytes.size() - off));
            if (!s->send_packet(Data, b)) {
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
    QTimer retry, timeout;
    QString host, id;
    quint16 port = 0;
    stream::Type type;
    bool enabled = false, ready = false, gap = true;
    int attempt = 0;
    QByteArray partial;
    uint32_t total = 0;
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
        partial.clear();
        timeout.start(5000);
        socket.connectToHost(host, port);
    }
    void recover() {
        ready = false;
        gap = true;
        partial.clear();
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
            socket.abort();
            recover();
        });
        QObject::connect(&socket, &QTcpSocket::connected, o,
                         [this] { socket.send_packet(Subscribe, request(id, type)); });
        QObject::connect(&socket, &QTcpSocket::disconnected, o, [this] { recover(); });
        QObject::connect(&socket, &QTcpSocket::errorOccurred, o, [this] { recover(); });
        QObject::connect(&socket, &TcpBDSPSocket::packetReceived, o, [this](quint8 kind, QByteArray b) {
            if (!enabled)
                return;
            if (kind == Error) {
                enabled = false;
                retry.stop();
                timeout.stop();
                report(QString::fromUtf8(b));
                socket.abort();
                return;
            }
            if (kind == Ack) {
                if (b != request(id, type)) {
                    socket.abort();
                    return;
                }
                ready = true;
                attempt = 0;
                timeout.stop();
                report("Running");
                return;
            }
            if (kind != Data || !ready || b.size() < 9) {
                socket.abort();
                return;
            }
            auto n = get_integer(b, 0), off = get_integer(b, 4);
            auto chunk = b.mid(8);
            if (n > stream::MaxFrameBytes || n < 112 || uint64_t(off) + chunk.size() > n || chunk.size() > Chunk) {
                socket.abort();
                return;
            }
            if (off == 0) {
                partial.clear();
                total = n;
            }
            if (n != total || off != uint32_t(partial.size())) {
                socket.abort();
                return;
            }
            partial.append(chunk);
            if (partial.size() != total)
                return;
            stream::Frame f;
            std::string error;
            if (!stream::decode({reinterpret_cast<const uint8_t *>(partial.constData()), size_t(partial.size())}, f,
                                error) ||
                f.type != type) {
                report(QString::fromStdString(error));
                socket.abort();
                return;
            }
            partial.clear();
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
        });
    }
};
NetworkSubscriber::NetworkSubscriber(QObject *p) : QObject(p), d(std::make_unique<Impl>(this)) {}
NetworkSubscriber::~NetworkSubscriber() {
    stop();
}
void NetworkSubscriber::start(QString h, quint16 p, QString id, stream::Type t) {
    stop();
    d->host = std::move(h);
    d->port = p;
    d->id = std::move(id);
    d->type = t;
    d->enabled = true;
    d->attempt = 0;
    d->connect();
}
void NetworkSubscriber::stop() {
    d->enabled = false;
    d->retry.stop();
    d->timeout.stop();
    d->socket.abort();
    d->partial.clear();
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
