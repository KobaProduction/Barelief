#ifndef AGGREGATOR_SERVER_PROTOCOL_PACKET_HANDLER_H
#define AGGREGATOR_SERVER_PROTOCOL_PACKET_HANDLER_H

#include "BDSP/transceivers.h"
#include <QTcpSocket>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(tcp_bdsp_socket)

class TcpBDSPSocket final : public QTcpSocket {
    Q_OBJECT

public:
    explicit TcpBDSPSocket(QObject *parent = nullptr);

    ~TcpBDSPSocket() override;

    void send_data(uint8_t packet_id, uint8_t *data, size_t size);
    bool send_packet(quint8 packet_id, const QByteArray &payload);

Q_SIGNALS:
    // Owned payload is safe to deliver with a queued connection.
    void packetReceived(quint8 packet_id, QByteArray payload);
    void on_got_packet(BDSP::packet_context_t &packet_context, TcpBDSPSocket *socket);

    void on_bdsp_packet_parsing_error(BDSP::parse_packet_status_t status, TcpBDSPSocket *socket);

    void on_disconnected(TcpBDSPSocket *socket);

private:
    std::unique_ptr<BDSP::COBSZPETransceiver> _transceiver;
    QByteArray _encoded;
    using QTcpSocket::readyRead;
};


#endif // AGGREGATOR_SERVER_PROTOCOL_PACKET_HANDLER_H
