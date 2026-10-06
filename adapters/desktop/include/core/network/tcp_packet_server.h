#ifndef QT_VISUALIZATION_TCP_PACKET_SERVER_H
#define QT_VISUALIZATION_TCP_PACKET_SERVER_H

#include <QTcpServer>
#include <QObject>
#include "tcp_packet_socket.h"

class TcpBDSPServer : public QTcpServer {
Q_OBJECT
public:
    explicit TcpBDSPServer(QObject *parent = nullptr);

Q_SIGNALS:
    void tcp_bdsp_socket_ready(TcpBDSPSocket *socket);

protected:
    void incomingConnection(qintptr handle) override;
};

#endif //QT_VISUALIZATION_TCP_PACKET_SERVER_H
