#include "core/network/tcp_packet_socket.h"
#include "core/utils.h"

Q_LOGGING_CATEGORY(tcp_bdsp_socket, "tcp_socket.bdsp")

TcpBDSPSocket::TcpBDSPSocket(QObject *parent)
    : QTcpSocket(parent), _transport(*this,2*1024*1024), _channel(*this, _transport) {
    connect(&_channel, &desktop::QtPacketChannel::packetReceived, this,
            [this](quint8 id, QByteArray payload) {
        BDSP::packet_context_t context{};
        context.packet_id=id;
        context.data_ptr=reinterpret_cast<uint8_t *>(payload.data());
        context.size=static_cast<uint16_t>(payload.size());
        qCDebug(tcp_bdsp_socket, "Received BDSP packet. ID: %d size: %d from %s", id,
                context.size, qPrintable(convert_qt_socket_to_string_ip_port(this)));
        emit on_got_packet(context, this);
        emit packetReceived(id, payload);
    });
    connect(&_channel, &desktop::QtPacketChannel::parsingError, this,
            [this](BDSP::parse_packet_status_t status) {
        emit on_bdsp_packet_parsing_error(status,this);
    });
    connect(this, &QTcpSocket::disconnected, this, [this]() { emit on_disconnected(this); });
}

TcpBDSPSocket::~TcpBDSPSocket() = default;

void TcpBDSPSocket::send_data(const uint8_t packet_id, uint8_t *data, const size_t size) {
    if(size>UINT16_MAX || (size && !data))return;
    send_packet(packet_id,QByteArray(reinterpret_cast<const char *>(data),static_cast<qsizetype>(size)));
}

bool TcpBDSPSocket::send_packet(quint8 packet_id, const QByteArray &payload) {
    if(!isWritable() || packet_id>31 || payload.size()>UINT16_MAX)return false;
    if(bytesToWrite()>2*1024*1024+65536) { abort();return false; }
    return _channel.send_packet(packet_id,payload)==signal_contracts::Status::ok;
}
