#include "aggregator/capabilities.h"
#include <QHostAddress>
#include <QPointer>
#include <QTimer>
#include <utility>
#include "aggregator/packet_codec.h"

#include "core/network/tcp_packet_socket.h"
#include "core/utils.h"
#include "aggregator/aggregator_server.h"
#include "aggregator/utils.h"

Q_LOGGING_CATEGORY(aggregator_server, "aggregator.server")
namespace {
void log_received_registration(TcpBDSPSocket* socket) {
    qCDebug(aggregator_server, "Registration received from %s",
            qPrintable(convert_qt_socket_to_string_ip_port(socket)));
}
bool registration_socket_available(TcpBDSPSocket* previous,TcpBDSPSocket* socket,const QUuid& uuid) {
    if(!previous || !previous->isOpen() || previous==socket)return true;
    qCWarning(aggregator_server, "Socket %s tried to take control of a component with a uuid: %s",
              qPrintable(convert_qt_socket_to_string_ip_port(socket)), qPrintable(uuid.toString()));
    return false;
}
void retire_registered_socket(QPointer<TcpBDSPSocket> socket) {
    if(!socket)return;
    socket->abort();
    if(socket)socket->deleteLater();
}
}

AggregatorServer::AggregatorServer(QObject *parent) : TcpBDSPServer(parent) {
    connect(this, &TcpBDSPServer::tcp_bdsp_socket_ready, this, &AggregatorServer::_on_new_connection);
}

AggregatorServer::~AggregatorServer() {
    stop();
}

bool AggregatorServer::start(const uint16_t port, const uint32_t registration_timeout_ms) {
    if (this->isListening()) {
        return true;
    }

    _registration_timeout_ms = registration_timeout_ms;
    if (not this->listen(QHostAddress::Any, port)) {
        qCCritical(aggregator_server, "Error: failed to start server on port %d. Maybe already used...", port);
        return false;
    }

    qCInfo(aggregator_server, "Aggregator server started on port %d", port);
    _log_registration_timeout();

    return true;
}

void AggregatorServer::_log_registration_timeout() const {
    if (_registration_timeout_ms == 0)
        qCInfo(aggregator_server, "Registration timeout: disabled (clients can connect without time limit)");
    else
        qCInfo(aggregator_server, "Registration timeout: %d ms", _registration_timeout_ms);
}

void AggregatorServer::stop() {
    if (this->isListening()) {
        this->close();
        qCInfo(aggregator_server, "Aggregator server stopped");
    }

    _close_registered_sockets();
}

void AggregatorServer::_close_registered_sockets() {
    QList<QPointer<TcpBDSPSocket>> sockets;
    for (auto it=_sockets_uuid_map.cbegin();it!=_sockets_uuid_map.cend();++it)
        sockets.append(it.key());
    _sockets_uuid_map.clear();
    _registered_components_map.clear();
    _capabilities.clear();
    _offline_generation.clear();
    for (const auto &socket : std::as_const(sockets)) {
        if (!socket)
            continue;
        socket->disconnect(this);
        retire_registered_socket(socket);
    }
}

bool AggregatorServer::_register_component(TcpBDSPSocket *socket, const QUuid &uuid, const component_info_t &new_info,
                                           const uint16_t session_token) {
    log_received_registration(socket);

    if (new_info.type < ComponentType::data_source || new_info.type > ComponentType::core)
        return false;
    if (uuid.isNull() || (_sockets_uuid_map.value(socket).has_value() && _sockets_uuid_map.value(socket) != uuid))
        return false;
    if (_registered_components_map.contains(uuid)) {
        if (!_restore_registration(socket, uuid, new_info, session_token))
            return false;
    } else _add_registration(socket,uuid,new_info,session_token);
    _offline_generation[uuid] = ++_next_generation;
    _sockets_uuid_map[socket] = uuid;
    _update_registered_capabilities(socket, uuid, new_info);
    _send_registry_snapshot(socket, uuid);
    return true;
}
void AggregatorServer::_add_registration(TcpBDSPSocket* socket,const QUuid& uuid,
                                         const component_info_t& new_info,uint16_t session_token) {
        qCInfo(aggregator_server, "Registered new component %s",
               qPrintable(aggregator_utils::format_component_registration(
                   uuid, new_info.type, convert_qt_socket_to_string_ip_port(socket))));
        _registered_components_map[uuid] = {.socket = socket,
                                            .session_token = session_token,
                                            .info = {
                                                .type = new_info.type,
                                                .state = ComponentState::registered,
                                                .component_server_port = new_info.component_server_port,
                                                .address = new_info.address,
                                            }};
}

bool AggregatorServer::_restore_registration(TcpBDSPSocket *socket, const QUuid &uuid,
                                              const component_info_t &new_info, uint16_t session_token) {
        auto &[socket_before, last_token, info] = _registered_components_map[uuid];
        if (!_session_token_matches(socket,uuid,last_token,session_token))return false;

        if(!registration_socket_available(socket_before,socket,uuid))return false;

        socket_before = socket;
        info = new_info;
        info.state = ComponentState::registered;

        _registered_components_map[uuid].info.state = ComponentState::registered;

        qCInfo(aggregator_server, "Session restored for component uuid: %s, %s", qPrintable(uuid.toString()),
               qPrintable(convert_qt_socket_to_string_ip_port(socket)));
    return true;
}
bool AggregatorServer::_session_token_matches(TcpBDSPSocket* socket,const QUuid& uuid,
                                              uint16_t expected,uint16_t supplied) const {
    if(expected==supplied)return true;
    qCWarning(aggregator_server, "Invalid session token for component uuid: %s from %s",
              qPrintable(uuid.toString()), qPrintable(convert_qt_socket_to_string_ip_port(socket)));
    return false;
}

void AggregatorServer::_update_registered_capabilities(TcpBDSPSocket *socket, const QUuid &uuid,
                                                         const component_info_t &new_info) {
    if (_capabilities.contains(uuid)) {
        _capabilities[uuid]["state"] = int(ComponentState::registered);
        _capabilities[uuid]["address"] = socket->peerAddress().toString();
        _capabilities[uuid]["component_type"] = int(new_info.type);
        _capabilities[uuid]["engine_port"] = new_info.component_server_port;
        _capabilities[uuid]["control_available"] =
            new_info.type == ComponentType::core && new_info.component_server_port > 0;
    }
}

void AggregatorServer::_send_registry_snapshot(TcpBDSPSocket *socket, const QUuid &uuid) {
    const auto new_info = _registered_components_map.value(uuid).info;
    component_update_full_packet_t info = {.uuid = uuid.toBytes(),
                                           .type = new_info.type,
                                           .address = socket->peerAddress().toIPv6Address(),
                                           .component_server_port = new_info.component_server_port,
                                           .state = ComponentState::registered};
    QPointer<TcpBDSPSocket> receiver(socket);
    _send_update_component_event(info);
    const auto registry = _registered_components_map;
    _send_registry_entries(receiver,uuid,registry);
    for (const auto &metadata : std::as_const(_capabilities))
        if (receiver)
            receiver->send_packet(aggregator_capabilities::PacketId, aggregator_capabilities::encode(metadata));
}
void AggregatorServer::_send_registry_entries(QPointer<TcpBDSPSocket> receiver,const QUuid& uuid,
                                             const QHash<QUuid,registered_component_t>& registry) {
    for (auto it = registry.cbegin(); it != registry.cend(); ++it) {
        if (!receiver || !_get_registration_status(receiver))
            break;
        if (it.key() == uuid)
            continue;
        const auto &entry = it.value().info;
        component_update_full_packet_t snapshot{.uuid = it.key().toBytes(),
                                                .type = entry.type,
                                                .address = entry.address.toIPv6Address(),
                                                .component_server_port = entry.component_server_port,
                                                .state = entry.state};
        if (!receiver->send_packet(static_cast<uint8_t>(PacketType::component_info_update),
                                   aggregator_wire::encode(snapshot)))
            break;
    }
}

void AggregatorServer::_reject(TcpBDSPSocket *socket, const QString &error) {
    QPointer<TcpBDSPSocket> receiver(socket);
    auto data = error.toUtf8().left(512);
    socket->send_data(static_cast<uint8_t>(PacketType::error), reinterpret_cast<uint8_t *>(data.data()), data.size());
    if (receiver)
        receiver->disconnectFromHost(); // Flush the explicit error before closing.
}

bool AggregatorServer::_get_registration_status(TcpBDSPSocket *socket) {
    return _sockets_uuid_map.value(socket).has_value();
}

void AggregatorServer::_disconnect_socket(TcpBDSPSocket *socket) {
    const auto entry = _sockets_uuid_map.find(socket);
    if (entry == _sockets_uuid_map.end())
        return;
    const auto uuid = entry.value();
    // Retire before broadcasting: send_packet may synchronously disconnect peers.
    _sockets_uuid_map.erase(entry);
    QPointer<TcpBDSPSocket> retiring(socket);
    socket->disconnect(this);
    if (uuid.has_value() and _registered_components_map.contains(uuid.value()) &&
        _registered_components_map.value(uuid.value()).socket == socket) {
        _registered_components_map[uuid.value()].socket = nullptr;
        _start_offline_timer(uuid.value());
        _update_component_state_with_broadcast(uuid.value(), ComponentState::disconnected);
    }
    if (retiring)
        retiring->abort();
    if (retiring)
        retiring->deleteLater();
}

void AggregatorServer::_on_got_packet(BDSP::packet_context_t &packet_context, TcpBDSPSocket *socket) {
    auto packet_type = static_cast<PacketType>(packet_context.packet_id);

    if (not _get_registration_status(socket) and packet_type not_eq PacketType::component_registration) {
        qCWarning(aggregator_server, "An unregistered socket (%s) tries to work without registration",
                  qPrintable(convert_qt_socket_to_string_ip_port(socket)));
        _reject(socket, "Registration required");
        return;
    }

    _dispatch_registered_packet(packet_context,socket,packet_type);
}
void AggregatorServer::_dispatch_registered_packet(BDSP::packet_context_t& packet_context,
                                                   TcpBDSPSocket* socket,PacketType packet_type) {
    switch (packet_type) {
    case static_cast<PacketType>(aggregator_capabilities::PacketId):
        _receive_capabilities(packet_context, socket); break;
    case PacketType::component_registration:
        _receive_registration(packet_context, socket); break;
    case PacketType::component_state_change:
        _receive_state_change(packet_context, socket); break;
    default:
        qCWarning(aggregator_server, "Unknown packet type %u from %s", packet_type,
                  qPrintable(convert_qt_socket_to_string_ip_port(socket)));
        _reject(socket, "Unknown packet type");
        break;
    }
}

void AggregatorServer::_receive_capabilities(BDSP::packet_context_t &packet_context, TcpBDSPSocket *socket) {
    QJsonObject metadata;
    if (!aggregator_capabilities::decode(
            QByteArray(reinterpret_cast<const char *>(packet_context.data_ptr), packet_context.size), metadata) ||
        QUuid(metadata["uuid"].toString()) != _sockets_uuid_map.value(socket).value()) {
        _reject(socket, "Invalid capabilities identity/version");
        return;
    }
    const QUuid uuid(metadata["uuid"].toString());
    metadata["address"] = socket->peerAddress().toString();
    const auto previous = _capabilities.value(uuid);
    if (previous.value("publisher_incarnation") == metadata.value("publisher_incarnation") &&
        previous.value("graph_revision") == metadata.value("graph_revision") &&
        !aggregator_capabilities::same_stream_bindings(previous.value("streams").toArray(),
                                                        metadata.value("streams").toArray())) {
        _reject(socket, "Canonical stream bindings cannot change within a publisher incarnation");
        return;
    }
    const auto component = _registered_components_map[uuid].info;
    metadata["state"] = int(component.state);
    metadata["component_type"] = int(component.type);
    metadata["engine_port"] = component.component_server_port;
    metadata["control_available"] = component.type == ComponentType::core && component.component_server_port > 0;
    _capabilities[uuid] = metadata;
    auto data = aggregator_capabilities::encode(metadata);
    _broadcast(static_cast<PacketType>(aggregator_capabilities::PacketId),
               reinterpret_cast<uint8_t *>(data.data()), data.size());
}

void AggregatorServer::_receive_registration(BDSP::packet_context_t &packet_context, TcpBDSPSocket *socket) {
    if (packet_context.size not_eq sizeof(registration_request_packet_t)) {
        qCWarning(aggregator_server, "Registration packet size incorrect from %s",
                  qPrintable(convert_qt_socket_to_string_ip_port(socket)));
        _reject(socket, "Invalid packet size");
        return;
    }
    registration_request_packet_t decoded{};
    if (!aggregator_wire::decode(
            QByteArray(reinterpret_cast<const char *>(packet_context.data_ptr), packet_context.size), decoded)) {
        _reject(socket, "Invalid registration");
        return;
    }
    const auto [uuid_raw, component_type, component_server_port, session_token] = decoded;
    const QUuid uuid = QUuid::fromBytes(&uuid_raw);
    const component_info_t info = {.type = component_type,
                                   .state = ComponentState::unknown,
                                   .component_server_port = component_server_port,
                                   .address = socket->peerAddress()};
    if (!_register_component(socket, uuid, info, session_token))
        _reject(socket, "Registration rejected: invalid identity, type or session");
}

void AggregatorServer::_receive_state_change(BDSP::packet_context_t &packet_context, TcpBDSPSocket *socket) {
    component_update_state_packet_t decoded{};
    if(!_decode_state_change(packet_context,socket,decoded))return;
    const auto [uuid_raw, new_state] = decoded;
    const auto uuid = QUuid::fromBytes(&uuid_raw);
    if (_sockets_uuid_map.value(socket) not_eq uuid) {
        qCWarning(aggregator_server, "Component tried to change state of another component");
        _reject(socket, "Cannot change another component state");
        return;
    }
    if (new_state < ComponentState::inactive || new_state > ComponentState::active) {
        _reject(socket, "Invalid component state");
        return;
    }
    _update_component_state_with_broadcast(uuid, new_state);
}
bool AggregatorServer::_decode_state_change(BDSP::packet_context_t& packet_context,TcpBDSPSocket* socket,
                                            component_update_state_packet_t& decoded) {
    if (packet_context.size not_eq sizeof(component_update_state_packet_t)) {
        qCWarning(aggregator_server, "Component state change packet size incorrect from %s",
                  qPrintable(convert_qt_socket_to_string_ip_port(socket)));
        _reject(socket, "Invalid packet size");
        return false;
    }
    if (!aggregator_wire::decode(
            QByteArray(reinterpret_cast<const char *>(packet_context.data_ptr), packet_context.size), decoded)) {
        _reject(socket, "Invalid state");
        return false;
    }
    return true;
}

void AggregatorServer::_on_new_connection(TcpBDSPSocket *socket) {
    qCDebug(aggregator_server, "new connection, %s", qPrintable(convert_qt_socket_to_string_ip_port(socket)));
    _sockets_uuid_map[socket].reset();

    connect(socket, &TcpBDSPSocket::on_disconnected, this, [this](TcpBDSPSocket *disconnected_socket) {
        qCDebug(aggregator_server, "disconnected, %s",
                qPrintable(convert_qt_socket_to_string_ip_port(disconnected_socket)));
        _disconnect_socket(disconnected_socket);
    });
    connect(socket, &TcpBDSPSocket::on_got_packet, this, &AggregatorServer::_on_got_packet);
    connect(socket, &TcpBDSPSocket::on_bdsp_packet_parsing_error, this,
            [this, socket] { _reject(socket, "Malformed packet"); });
    _schedule_registration_timeout(socket);
}

void AggregatorServer::_schedule_registration_timeout(TcpBDSPSocket *socket) {
    if (not _registration_timeout_ms) {
        qCDebug(aggregator_server, "Registration timeout disabled for %s",
                qPrintable(convert_qt_socket_to_string_ip_port(socket)));
        return;
    }
    // Устанавливаем таймер регистрации только если timeout не равен 0
    QTimer::singleShot(_registration_timeout_ms, socket, [socket, this]() {
        if (this->_get_registration_status(socket))
            return;
        qCWarning(aggregator_server, "Registration timeout for %s",
                  qPrintable(convert_qt_socket_to_string_ip_port(socket)));
        _reject(socket, "Registration timeout");
    });
}

void AggregatorServer::_send_update_component_event(component_update_full_packet_t &info) {
    auto encoded = aggregator_wire::encode(info);
    _broadcast(PacketType::component_info_update, reinterpret_cast<uint8_t *>(encoded.data()), encoded.size());
}

void AggregatorServer::_broadcast(const PacketType packet_type, uint8_t *data, const size_t size) {
    _broadcast_except_socket(packet_type, data, size, nullptr);
}

void AggregatorServer::_broadcast_except_socket(PacketType packet_type, uint8_t *data, const size_t size,
                                                const TcpBDSPSocket *except_socket) {
    QList<QPointer<TcpBDSPSocket>> recipients;
    for (auto it = _sockets_uuid_map.cbegin(); it != _sockets_uuid_map.cend(); ++it) {
        if (it.key() != except_socket && it.value().has_value())
            recipients.append(it.key());
    }
    for (const auto &socket : recipients) {
        if (socket && _get_registration_status(socket))
            socket->send_data(static_cast<uint8_t>(packet_type), data, size);
    }
}

void AggregatorServer::_send_component_state_change_event(const QUuid &uuid, ComponentState new_state) {
    component_update_state_packet_t state_data = {.uuid = uuid.toBytes(), .new_state = new_state};

    qCDebug(aggregator_server, "Broadcasting component state change for %s",
            qPrintable(aggregator_utils::format_component_state_change(uuid, new_state)));
    auto encoded = aggregator_wire::encode(state_data);
    _broadcast(PacketType::component_state_change, reinterpret_cast<uint8_t *>(encoded.data()), encoded.size());
}

void AggregatorServer::_update_component_state_with_broadcast(const QUuid &uuid, const ComponentState new_state) {
    if (_registered_components_map.contains(uuid)) {
        _registered_components_map[uuid].info.state = new_state;
        if (_capabilities.contains(uuid))
            _capabilities[uuid]["state"] = int(new_state);
    }
    // Уведомляем всех клиентов об изменении состояния через component_state_change
    _send_component_state_change_event(uuid, new_state);
    qCInfo(aggregator_server, "Component state changed to %s for %s",
           qPrintable(aggregator_utils::component_state_to_string(new_state)), qPrintable(uuid.toString()));
}

void AggregatorServer::_start_offline_timer(const QUuid &uuid) {
    qCDebug(aggregator_server, "Starting offline timer for component %s (%d ms)", qPrintable(uuid.toString()),
            aggregator_component::COMPONENT_OFFLINE_TIMEOUT_MS);

    const auto generation = _offline_generation[uuid] = ++_next_generation;
    QTimer::singleShot(aggregator_component::COMPONENT_OFFLINE_TIMEOUT_MS, this, [this, uuid, generation]() {
        _expire_disconnected_component(uuid, generation);
    });
}

void AggregatorServer::_expire_disconnected_component(const QUuid &uuid, uint64_t generation) {
    if (!_registered_components_map.contains(uuid) || _offline_generation.value(uuid) != generation ||
        _registered_components_map.value(uuid).info.state != ComponentState::disconnected)
        return;
    qCInfo(aggregator_server, "Component %s went offline due to timeout", qPrintable(uuid.toString()));
    _send_component_state_change_event(uuid, ComponentState::offline);
    if (_offline_generation.value(uuid) == generation) {
        _registered_components_map.remove(uuid);
        _offline_generation.remove(uuid);
    }
    qCInfo(aggregator_server, "Component %s removed from registry", qPrintable(uuid.toString()));
}
