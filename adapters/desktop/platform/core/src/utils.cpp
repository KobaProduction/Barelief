#include "core/utils.h"

#include <QUdpSocket>
#include <QNetworkInterface>

QString convert_qt_host_address_to_string(const QHostAddress &address) {
    bool is_ipv4;
    const QHostAddress ip4Address(address.toIPv4Address(&is_ipv4));
    return is_ipv4 ? ip4Address.toString() : address.toString();;
}

QString convert_qt_socket_to_string_ip_port(const QAbstractSocket *socket) {
    const QString address = convert_qt_host_address_to_string(socket->peerAddress());
    return QString("ip: %1, port: %2").arg(address).arg(socket->peerPort());
}

QHostAddress get_default_interfaces() {
    QHostAddress defaultInterfaceAddress;
    QUdpSocket testSocket;
    testSocket.connectToHost("1.1.1.1", 53);
    if (testSocket.waitForConnected(100)) {
        defaultInterfaceAddress = testSocket.localAddress();
    }
    testSocket.abort();
    return defaultInterfaceAddress;
}

QList<NetworkInterfaceEntry> get_available_network_interfaces() {
    QList<NetworkInterfaceEntry> entries;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    
    for (const auto& interface : interfaces) {
        if (not interface.flags().testFlag(QNetworkInterface::IsUp) or 
            interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        
        const QList<QNetworkAddressEntry> addressEntries = interface.addressEntries();
        for (const auto& addressEntry : addressEntries) {
            if (addressEntry.ip().protocol() not_eq QAbstractSocket::IPv4Protocol) {
                continue;
            }
            
            entries.append({
                interface.name(),
                interface.humanReadableName(),
                addressEntry.ip()
            });
        }
    }
    
    return entries;
}

QHostAddress get_network_interface_address(const QString &interface_name) {
    if (interface_name.isEmpty()) {
        return get_default_interfaces();
    }
    
    const auto entries = get_available_network_interfaces();
    for (const auto& entry : entries) {
        if (entry.interface_name == interface_name) {
            return entry.ip_address;
        }
    }
    
    return get_default_interfaces();
}


//network_address_t convert_qt_host_address_to_network_address(const QHostAddress &address) {
//    network_address_t network_address{};
//    bool is_ipv4;
//    network_address.ipv4 = address.toIPv4Address(&is_ipv4);
//    network_address.ip_version = is_ipv4 ? 4 : 6;
//    if (is_ipv4) return network_address;
//    auto ipv6 = address.toIPv6Address();
//    memcpy(network_address.bytes, &ipv6, 16);
//    return network_address;
//}
