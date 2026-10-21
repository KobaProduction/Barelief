#ifndef AGGREGATOR_CLIENT_H
#define AGGREGATOR_CLIENT_H

#include <QObject>
#include <QHostAddress>
#include <QJsonObject>
#include <QMap>
#include <QTimer>
#include "core/runtime_state.h"

#include <BDSP/transceivers.h>

#include "aggregator/abstract/types.h"
#include "core/network/tcp_packet_socket.h"

Q_DECLARE_LOGGING_CATEGORY(aggregator_client)

struct AggregatorClientConfig {
    QUuid uuid{};
    ComponentType type = ComponentType::unknown;
    uint16_t component_server_port = 0;
    std::optional<session_token_t> session_token; // Persisted by the composition root for process restart.
};

/// @brief Клиент агрегатора с поддержкой событийной системы.
///
/// Подключается к серверу агрегатора, регистрирует компонент и получает
/// уведомления о состоянии других компонентов системы. Реализует интерфейсы
/// для публикации событий, управления жизненным циклом и фокусом окон.
class AggregatorClient final : public QObject {
    Q_OBJECT

  public:
    /// @brief Конструктор клиента агрегатора
    /// @param config Конфигурация клиента
    /// @param parent Родительский объект Qt
    explicit AggregatorClient(const AggregatorClientConfig &config, QObject *parent = nullptr);

    /// @brief Деструктор
    ~AggregatorClient() override;

    /// @brief Подключение к серверу агрегатора
    /// @param address IP-адрес сервера агрегатора
    /// @param port Порт сервера агрегатора
    /// @param timeout_ms Тайм-аут подключения в миллисекундах
    /// @return true если асинхронное подключение запрошено; результат приходит через сигналы
    bool connect_to_aggregator_server(QHostAddress address = QHostAddress::LocalHost,
                                      uint16_t port = aggregator_component::DEFAULT_PORT,
                                      int32_t timeout_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS);

    /// @brief Асинхронное подключение с бесконечными повторными попытками
    /// @param address IP-адрес сервера агрегатора
    /// @param port Порт сервера агрегатора
    /// @param timeout_ms Тайм-аут подключения в миллисекундах
    void
    connect_to_aggregator_server_with_waiting(const QHostAddress &address = QHostAddress::LocalHost,
                                              uint16_t port = aggregator_component::DEFAULT_PORT,
                                              int32_t timeout_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS);

    /// @brief Проверка подключения к серверу
    /// @return true если подключен, false в противном случае
    bool is_connected() const;
    void disconnect_from_aggregator_server();
    core::RuntimeState runtime_state() const {
        return _state;
    }
    QString error() const {
        return _error;
    }
    bool set_capabilities(QJsonObject capabilities);
    QMap<QUuid, QJsonObject> capabilities() const {
        return _capabilities;
    }
    QMap<QUuid, component_info_t> components() const {
        return _components;
    }
    // Returns only a currently discoverable core control endpoint, never a stream port.
    QJsonObject resolve_engine(const QUuid &uuid, QString *error = nullptr) const;
    QString discovery_status() const;

    /// @brief Проверка регистрации клиента как компонента
    /// @return true если зарегистрирован, false в противном случае
    bool is_registered() const;
    struct ConnectionRequest {QHostAddress address;quint16 port=0;qint32 timeout_ms=0;};
    // Read only on this QObject's thread. A worker observer mirrors immutable
    // explicit intent, never this object's mutable discovery/cache state.
    std::optional<ConnectionRequest> connection_request() const {return _connection_request;}

    /// @brief Настройка режима переподключения
    /// @param infinite_mode true для бесконечных попыток переподключения
    /// @param tries Количество попыток переподключения (если не бесконечный режим)
    /// @param reconnect_interval_ms Интервал между попытками переподключения
    void set_reconnect_mode(bool infinite_mode, int32_t tries = aggregator_component::RECONNECT_TRIES,
                            int32_t reconnect_interval_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS);

  Q_SIGNALS:
    void connection_requested(QHostAddress address,quint16 port,qint32 timeout_ms);
    void disconnection_requested();
    void status_changed(core::RuntimeState state, const QString &error);
    /// @brief Сигнал о закрытии клиента
    void on_client_disconnected();
    void capabilities_changed(const QUuid &uuid, const QJsonObject &capabilities);

    /// @brief Сигнал о подключении клиента к агрегатору
    void on_client_connected();

    /// @brief Сигнал о регистрации клиента как компонента
    void on_client_registered();

    /// @brief Сигнал об изменении состояния компонента
    /// @param uuid UUID компонента
    /// @param new_state Новое состояние
    void on_update_component_state(const QUuid &uuid, ComponentState new_state);

    /// @brief Сигнал об обновлении компонента
    /// @param uuid UUID компонента
    /// @param info Информация о компоненте
    void on_update_component_info(const QUuid &uuid, const component_info_t &info);

  private:
    AggregatorClientConfig _cfg{};
    std::optional<ConnectionRequest> _connection_request;
    QJsonObject _own_capabilities;
    QMap<QUuid, QJsonObject> _capabilities;
    QMap<QUuid, component_info_t> _components;
    TcpBDSPSocket _socket;
    QHostAddress _aggregator_server_address = QHostAddress::LocalHost;
    uint16_t _aggregator_server_port = aggregator_component::DEFAULT_PORT;
    bool _is_registered = false;
    session_token_t _session_token = 0;
    std::optional<int32_t> _reconnect_tries;
    int32_t _current_reconnect_attempt = 0;
    QTimer _reconnect_timer, _timeout_timer;
    bool _enabled = false, _waiting_for_first_registration = false;
    std::optional<int32_t> _saved_reconnect_tries;
    int32_t _timeout_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS;
    core::RuntimeState _state = core::RuntimeState::Stopped;
    QString _error;
    void _begin_connection();
    void _set_status(core::RuntimeState state, QString error = {});
    void _invalidate_discovery();
    int32_t _reconnect_interval_ms = aggregator_component::CLIENT_RECONNECT_TIMEOUT_MS;

    void _handle_packet(BDSP::packet_context_t &packet_ctx);
    void _bind_socket_signals();
    void _on_connected();
    void _on_disconnected();
    void _on_socket_error();
    void _on_timeout();
    void _handle_capabilities(BDSP::packet_context_t &packet_ctx);
    void _handle_error(BDSP::packet_context_t &packet_ctx);
    void _record_component_state(const QUuid&,ComponentState);
    void _restore_registration_retry_policy();
    void _record_decoded_info(const component_update_full_packet_t&);
    QJsonObject _resolved_engine_metadata(const QUuid&) const;
    void _handle_state_change(BDSP::packet_context_t &packet_ctx);
    void _handle_info_update(BDSP::packet_context_t &packet_ctx);
    void _record_component_info(const QUuid& uuid,const component_info_t& info);
    void _record_self_registration(const component_info_t& info);
    QString _engine_resolution_error(const QUuid& uuid) const;

    void _start_reconnecting();

    void _send_registration_request();
};

#endif // AGGREGATOR_CLIENT_H
