#include "core/network/tcp_packet_server.h"

Q_LOGGING_CATEGORY(tcp_bdsp_server, "aggregator.tcp_bdsp_server")

TcpBDSPServer::TcpBDSPServer(QObject *parent) : QTcpServer(parent) {};

void TcpBDSPServer::incomingConnection(qintptr handle) {
    auto *socket = new TcpBDSPSocket(this);  // parent = server
    if (not socket->setSocketDescriptor(handle)) {
        qCWarning(tcp_bdsp_server, "Failed to set _socket descriptor:: %s", qPrintable(socket->errorString()));
        delete socket;
        return;
    }
    emit tcp_bdsp_socket_ready(socket);
}

