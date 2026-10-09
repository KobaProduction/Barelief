#include "aggregator/capabilities.h"
#include <QTimer>
#include "aggregator/packet_codec.h"

#include "core/utils.h"
#include "aggregator/aggregator_client.h"
#include "aggregator/utils.h"

Q_LOGGING_CATEGORY(aggregator_client, "aggregator.client")

AggregatorClient::AggregatorClient(const AggregatorClientConfig &config, QObject *parent)
    : QObject(parent), _cfg(config), _socket(this), _reconnect_timer(this) {
    _session_token = _cfg.session_token.value_or(
        aggregator_utils::generate_session_token(_cfg.uuid, _cfg.type, _cfg.component_server_port));
    connect(&_socket, &TcpBDSPSocket::on_got_packet, this, &AggregatorClient::_handle_packet);

    _timeout_timer.setSingleShot(true);
    _reconnect_timer.setSingleShot(true);
    connect(&_socket, &QTcpSocket::connected, this, [this] {
        if (!_enabled) {
            _socket.abort();
            return;
        }
        _timeout_timer.start(aggregator_component::REGISTRATION_TIMEOUT_MS);
        emit on_client_connected();
        _send_registration_request();
    });
    connect(&_socket, &QTcpSocket::disconnected, this, [this] {
        _timeout_timer.stop();
        _invalidate_discovery();
        if (_is_registered) {
            _is_registered = false;
            emit on_client_disconnected();
        }
        _start_reconnecting();
    });
    connect(&_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        _timeout_timer.stop();
        _invalidate_discovery();
        _error = _socket.errorString();
        _start_reconnecting();
    });
    connect(&_timeout_timer, &QTimer::timeout, this, [this] {
        _error = is_connected() ? "Registration timeout" : "Connection timeout";
        _socket.abort();
        _start_reconnecting();
    });
    connect(&_reconnect_timer, &QTimer::timeout, this, [this] {
        ++_current_reconnect_attempt;
        _begin_connection();
    });

    qCDebug(aggregator_client, "AggregatorClient created for component: %s", qPrintable(_cfg.uuid.toString()));
}

AggregatorClient::~AggregatorClient() {
    _enabled = false;
    _reconnect_timer.stop();
    _timeout_timer.stop();
    _socket.disconnect(this);
    _socket.abort();
}
void AggregatorClient::_set_status(core::RuntimeState state, QString error) {
    if (_state == state && _error == error)
        return;
    _state = state;
    _error = std::move(error);
    emit status_changed(_state, _error);
}
void AggregatorClient::disconnect_from_aggregator_server() {
    _connection_request.reset();
    _enabled = false;
    _reconnect_timer.stop();
    _timeout_timer.stop();
    _socket.abort();
    _is_registered = false;
    _invalidate_discovery();
    _set_status(core::RuntimeState::Stopped);
    emit disconnection_requested();
}
bool AggregatorClient::connect_to_aggregator_server(QHostAddress address, uint16_t port, int32_t timeout_ms) {
    disconnect_from_aggregator_server();
    _waiting_for_first_registration = false;
    if (address.isNull() || !port || timeout_ms <= 0) {
        _set_status(core::RuntimeState::Failed, "Invalid connection parameters");
        return false;
    }
    _aggregator_server_address = std::move(address);
    _aggregator_server_port = port;
    _timeout_ms = timeout_ms;
    _enabled = true;
    _current_reconnect_attempt = 0;
    _connection_request=ConnectionRequest{_aggregator_server_address,port,timeout_ms};
    emit connection_requested(_aggregator_server_address,port,timeout_ms);
    _set_status(core::RuntimeState::Starting);
    _begin_connection();
    return true;
}
void AggregatorClient::_begin_connection() {
    if (!_enabled)
        return;
    _timeout_timer.start(_timeout_ms);
    _socket.connectToHost(_aggregator_server_address, _aggregator_server_port);
}
void AggregatorClient::connect_to_aggregator_server_with_waiting(const QHostAddress &address, uint16_t port,
                                                                 int32_t timeout_ms) {
    const auto saved = _reconnect_tries;
    _reconnect_tries.reset();
    connect_to_aggregator_server(address, port, timeout_ms);
    _saved_reconnect_tries = saved;
    _waiting_for_first_registration = true;
}

bool AggregatorClient::is_connected() const {
    return _socket.state() == QAbstractSocket::ConnectedState;
}

bool AggregatorClient::is_registered() const {
    return _is_registered;
}

void AggregatorClient::_start_reconnecting() {
    if (!_enabled || _reconnect_timer.isActive())
        return;
    if (_reconnect_tries && _current_reconnect_attempt >= *_reconnect_tries) {
        _enabled = false;
        _timeout_timer.stop();
        _socket.abort();
        _set_status(core::RuntimeState::Failed, _error.isEmpty() ? "Reconnect attempts exhausted" : _error);
        return;
    }
    _set_status(core::RuntimeState::Recovering, _error);
    const auto multiplier = 1 << std::min(_current_reconnect_attempt, 5);
    _reconnect_timer.start(std::min<int64_t>(30000, int64_t(_reconnect_interval_ms) * multiplier));
}

void AggregatorClient::_handle_packet(BDSP::packet_context_t &packet_context) {
    const auto packet_type = static_cast<PacketType>(packet_context.packet_id);
    switch (packet_type) {
    case static_cast<PacketType>(aggregator_capabilities::PacketId): {
        QJsonObject metadata;
        if (!aggregator_capabilities::decode(
                QByteArray(reinterpret_cast<const char *>(packet_context.data_ptr), packet_context.size), metadata))
            return;
        const QUuid uuid(metadata["uuid"].toString());
        metadata["registry_available"] = true;
        if (_components.contains(uuid)) {
            const auto info = _components.value(uuid);
            metadata["component_type"] = int(info.type);
            metadata["state"] = int(info.state);
            metadata["engine_port"] = info.component_server_port;
            metadata["control_available"] = info.type == ComponentType::core && info.component_server_port > 0;
        }
        _capabilities[uuid] = metadata;
        emit capabilities_changed(uuid, metadata);
        break;
    }
    case PacketType::error: {
        const auto message = QString::fromUtf8(reinterpret_cast<const char *>(packet_context.data_ptr),
                                               static_cast<qsizetype>(packet_context.size));
        _enabled = false;
        _reconnect_timer.stop();
        _timeout_timer.stop();
        _socket.abort();
        _set_status(core::RuntimeState::Failed, message);
        return;
    }
    case PacketType::component_state_change: {
        if (packet_context.size not_eq sizeof(component_update_state_packet_t)) {
            qCWarning(aggregator_client, "Component state change packet size incorrect.");
            return;
        }

        component_update_state_packet_t decoded{};
        if (!aggregator_wire::decode(
                QByteArray(reinterpret_cast<const char *>(packet_context.data_ptr), packet_context.size), decoded))
            return;
        const auto [uuid_bytes, new_state] = decoded;
        const auto uuid = QUuid::fromBytes(&uuid_bytes);

        if (uuid == _cfg.uuid)
            return;
        if (_components.contains(uuid))
            _components[uuid].state = new_state;
        qCInfo(aggregator_client, "Component state change: %s",
               qPrintable(aggregator_utils::format_component_state_change(uuid, new_state)));
        if (_capabilities.contains(uuid)) {
            _capabilities[uuid]["state"] = int(new_state);
            emit capabilities_changed(uuid, _capabilities[uuid]);
        }
        emit on_update_component_state(uuid, new_state);
        break;
    }
    case PacketType::component_info_update: {
        if (packet_context.size not_eq sizeof(component_update_full_packet_t)) {
            qCWarning(aggregator_client, "Component info packet size incorrect.");
            return;
        }

        component_update_full_packet_t decoded{};
        if (!aggregator_wire::decode(
                QByteArray(reinterpret_cast<const char *>(packet_context.data_ptr), packet_context.size), decoded))
            return;
        const auto [uuid_raw, type, address_raw, component_server_port, state] = decoded;
        const QUuid uuid = QUuid::fromBytes(&uuid_raw);

        const component_info_t info{
            .type = type,
            .state = state,
            .component_server_port = component_server_port,
            .address = QHostAddress(address_raw),
        };
        if (uuid != _cfg.uuid) {
            _components[uuid] = info;
            auto metadata = _capabilities.value(uuid);
            metadata["uuid"] = uuid.toString();
            metadata["address"] = info.address.toString();
            metadata["state"] = int(info.state);
            metadata["component_type"] = int(info.type);
            metadata["engine_port"] = info.component_server_port;
            metadata["control_available"] = info.type == ComponentType::core && info.component_server_port > 0;
            metadata["registry_available"] = true;
            _capabilities[uuid] = metadata;
            emit capabilities_changed(uuid, metadata);
        }

        if (uuid == _cfg.uuid) {
            if (state >= ComponentState::registered) {
                if (_is_registered)
                    return;
                _is_registered = true;
                if (_waiting_for_first_registration) {
                    _reconnect_tries = _saved_reconnect_tries;
                    _waiting_for_first_registration = false;
                }
                _current_reconnect_attempt = 0;
                _reconnect_timer.stop();
                _timeout_timer.stop();
                _set_status(core::RuntimeState::Running);
                qCInfo(aggregator_client, "Registration AggregatorClient successful! External Ip: %s",
                       qPrintable(convert_qt_host_address_to_string(info.address)));
                emit on_client_registered();
                if (!_own_capabilities.isEmpty())
                    _socket.send_packet(aggregator_capabilities::PacketId,
                                        aggregator_capabilities::encode(_own_capabilities));
            }
            return;
        }

        qCInfo(aggregator_client, "New component: %s",
               qPrintable(aggregator_utils::format_component_info(uuid, type, state)));

        // Генерируем событие обновления компонента
        emit on_update_component_info(uuid, info);
        break;
    }
    default:
        qCWarning(aggregator_client, "Got unknown packet type: %d", packet_type);
        break;
    }
}

void AggregatorClient::set_reconnect_mode(const bool infinite_mode, int32_t tries,
                                          const int32_t reconnect_interval_ms) {
    _reconnect_interval_ms = std::max(1, reconnect_interval_ms);
    if (infinite_mode) {
        _reconnect_tries.reset();
        return;
    }
    _reconnect_tries = std::max(0, tries);
}

void AggregatorClient::_send_registration_request() {
    registration_request_packet_t data{};
    data.component_type = _cfg.type;
    data.component_server_port = _cfg.component_server_port;
    data.uuid = _cfg.uuid.toBytes();
    data.session_token = _session_token;
    _socket.send_packet(static_cast<uint8_t>(PacketType::component_registration), aggregator_wire::encode(data));
}

void AggregatorClient::set_capabilities(QJsonObject metadata) {
    metadata["version"] = 3;
    metadata["uuid"] = _cfg.uuid.toString(QUuid::WithoutBraces);
    if (!aggregator_capabilities::valid(metadata))
        return;
    _own_capabilities = metadata;
    if (_is_registered)
        _socket.send_packet(aggregator_capabilities::PacketId, aggregator_capabilities::encode(metadata));
}

void AggregatorClient::_invalidate_discovery() {
    for (auto it = _components.begin(); it != _components.end(); ++it) {
        it->state = ComponentState::unknown; // The directory is unavailable; the device itself may still be running.
        auto metadata = _capabilities.value(it.key());
        metadata["state"] = int(ComponentState::unknown);
        metadata["registry_available"] = false;
        _capabilities[it.key()] = metadata;
        emit capabilities_changed(it.key(), metadata);
        emit on_update_component_state(it.key(), ComponentState::unknown);
    }
}
QString AggregatorClient::discovery_status() const {
    const auto endpoint = QString("%1:%2").arg(_aggregator_server_address.toString()).arg(_aggregator_server_port);
    if (_state == core::RuntimeState::Starting)
        return "Connecting to registry at " + endpoint;
    if (is_registered() && is_connected() && _state == core::RuntimeState::Running) {
        int engines = 0;
        for (const auto &info : _components)
            if (info.type == ComponentType::core && info.state >= ComponentState::registered &&
                info.component_server_port)
                ++engines;
        return engines ? QString("Registry connected at %1; %2 engine endpoint(s)").arg(endpoint).arg(engines)
                       : "Registry connected; no core control endpoints are registered. Start a core with an enabled "
                         "engine server and registry registration, or enter its direct endpoint.";
    }
    if (_state == core::RuntimeState::Stopped)
        return "Registry disconnected. Connect to a configured registry or enter a direct engine endpoint. Local "
               "processing does not require a registry.";
    return QString("Registry unavailable at %1: %2. Start or configure an aggregator at this address, or enter a "
                   "direct engine endpoint.")
        .arg(endpoint, _error.isEmpty() ? QString("connection lost") : _error);
}
QJsonObject AggregatorClient::resolve_engine(const QUuid &uuid, QString *error) const {
    auto failure = [&](QString message) {
        if (error)
            *error = std::move(message);
        return QJsonObject{};
    };
    if (!is_registered() || !is_connected() || _state != core::RuntimeState::Running)
        return failure(discovery_status());
    if (!_components.contains(uuid))
        return failure(
            "Core is not registered in this directory. Refresh discovery or enter a direct engine endpoint.");
    const auto info = _components.value(uuid);
    if (info.type != ComponentType::core)
        return failure("This component is not a processing core.");
    if (info.state < ComponentState::registered)
        return failure("Core endpoint is unavailable. Wait for reconnection or enter another engine endpoint.");
    if (!info.component_server_port)
        return failure("This core publishes streams but has no control endpoint. Enable its engine server to open it "
                       "for remote viewing or editing.");
    auto result = _capabilities.value(uuid);
    if(!aggregator_capabilities::valid(result))
        return failure("Canonical numeric CONTROL/DATA endpoint metadata is unavailable.");
    result["uuid"] = uuid.toString();
    result["address"] = info.address.toString();
    result["port"] = info.component_server_port;
    result["engine_port"] = info.component_server_port;
    result["component_type"] = int(info.type);
    result["state"] = int(info.state);
    result["registry_available"] = true;
    result["control_available"] = true;
    if (error)
        error->clear();
    return result;
}
