#include "core/network/tcp_packet_socket.h"
#include "core/utils.h"

#include <QDataStream>

Q_LOGGING_CATEGORY(tcp_bdsp_socket, "tcp_socket.bdsp")

TcpBDSPSocket::TcpBDSPSocket(QObject *parent)
    : QTcpSocket(parent), _transceiver(std::make_unique<BDSP::COBSZPETransceiver>()) {
    _transceiver->get_write_stream().set_stream_writer(
        [](const uint8_t byte, void *ctx) {
            auto *socket = static_cast<TcpBDSPSocket *>(ctx);
            socket->_encoded.append(static_cast<char>(byte));
        },
        this);

    _transceiver->set_packet_handler(
        [](BDSP::packet_context_t &packet_ctx, void *ctx) {
            const auto socket = static_cast<TcpBDSPSocket *>(ctx);
            qCDebug(tcp_bdsp_socket, "Received BDSP packet. ID: %d size: %d from %s", packet_ctx.packet_id,
                    packet_ctx.size, qPrintable(convert_qt_socket_to_string_ip_port(socket)));
            emit socket->on_got_packet(packet_ctx, socket);
            emit socket->packetReceived(
                packet_ctx.packet_id, QByteArray(reinterpret_cast<const char *>(packet_ctx.data_ptr), packet_ctx.size));
        },
        this);

    _transceiver->set_error_handler(
        [](const BDSP::parse_packet_status_t status, void *ctx) {
            const auto tcp_bdsp_socket = static_cast<TcpBDSPSocket *>(ctx);
            emit tcp_bdsp_socket->on_bdsp_packet_parsing_error(status, tcp_bdsp_socket);
        },
        this);
    connect(this, &QTcpSocket::readyRead, this, [this]() {
        QByteArray data = this->readAll();
        if (data.isEmpty())
            return;
        _transceiver->get_read_stream().read(reinterpret_cast<uint8_t *>(data.data()), data.size());
    });

    connect(this, &QTcpSocket::connected, this, [this] { _transceiver->reset(false); });
    connect(this, &QTcpSocket::disconnected, this, [this]() { emit this->on_disconnected(this); });
}

TcpBDSPSocket::~TcpBDSPSocket() = default;

void TcpBDSPSocket::send_data(const uint8_t packet_id, uint8_t *data, const size_t size) {
    if (size > UINT16_MAX || (size && !data))
        return;
    send_packet(packet_id, QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size)));
}

bool TcpBDSPSocket::send_packet(quint8 packet_id, const QByteArray &payload) {
    if (!isWritable() || packet_id > 31 || payload.size() > UINT16_MAX)
        return false;
    // Engine limits unencoded messages to 2 MiB; reserve room for COBS framing.
    if (bytesToWrite() > 2 * 1024 * 1024 + 65536) {
        abort();
        return false;
    }
    _encoded.clear();
    auto owned = payload;
    const auto status = _transceiver->send_data(packet_id, reinterpret_cast<uint8_t *>(owned.data()),
                                                static_cast<size_t>(owned.size()));
    if (status != BDSP::SEND_PACKET_OK)
        return false;
    return write(_encoded) == _encoded.size();
}
