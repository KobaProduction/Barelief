#ifndef QT_VISUALIZATION_UTILS_H
#define QT_VISUALIZATION_UTILS_H

#include <QString>
#include <QHostAddress>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QNetworkAddressEntry>

QString convert_qt_host_address_to_string(const QHostAddress &address);
QString convert_qt_socket_to_string_ip_port(const QAbstractSocket *socket);

QHostAddress get_default_interfaces();

struct NetworkInterfaceEntry {
    QString interface_name;
    QString human_readable_name;
    QHostAddress ip_address;
};

QList<NetworkInterfaceEntry> get_available_network_interfaces();

QHostAddress get_network_interface_address(const QString &interface_name);

#endif //QT_VISUALIZATION_UTILS_H
