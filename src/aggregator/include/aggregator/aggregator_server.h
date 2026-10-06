#ifndef AGGREGATOR_SERVER_H
#define AGGREGATOR_SERVER_H

#include <QObject>
#include <QHash>

#include "abstract/types.h"
#include "core/network/tcp_packet_socket.h"
#include "core/network/tcp_packet_server.h"

Q_DECLARE_LOGGING_CATEGORY(aggregator_server)

/**
 * @brief Сервер агрегации событий
 *
 * Центральный компонент системы для отслеживания доступности
 * и состояния всех компонентов. Работает на статическом порту 8212.
 */

class AggregatorServer final : public TcpBDSPServer {
    Q_OBJECT

  public:
    explicit AggregatorServer(QObject *parent = nullptr);

    ~AggregatorServer() override;

    bool start(uint16_t port = aggregator_component::DEFAULT_PORT,
               uint32_t registration_timeout_ms = aggregator_component::REGISTRATION_TIMEOUT_MS);

    void stop();

  protected:
    QHash<TcpBDSPSocket *, std::optional<QUuid>> _sockets_uuid_map;
    QHash<QUuid, registered_component_t> _registered_components_map;
    QHash<QUuid, uint64_t> _offline_generation;
    uint64_t _next_generation = 0;
    uint32_t _registration_timeout_ms = aggregator_component::REGISTRATION_TIMEOUT_MS;

  protected Q_SLOTS:
    void _on_got_packet(BDSP::packet_context_t &packet_context, TcpBDSPSocket *socket);

    void _on_new_connection(TcpBDSPSocket *socket);

  private:
    QMap<QUuid, QJsonObject> _capabilities;
    bool _register_component(TcpBDSPSocket *socket, const QUuid &uuid, const component_info_t &new_info,
                             const uint16_t session_token);

    void _reject(TcpBDSPSocket *socket, const QString &error);

    bool _get_registration_status(TcpBDSPSocket *socket);

    void _disconnect_socket(TcpBDSPSocket *socket);

    void _send_update_component_event(component_update_full_packet_t &info);

    void _send_component_state_change_event(const QUuid &uuid, ComponentState new_state);

    void _broadcast(PacketType packet_type, uint8_t *data, size_t size);

    void _broadcast_except_socket(PacketType packet_type, uint8_t *data, size_t size,
                                  const TcpBDSPSocket *except_socket);

    void _update_component_state_with_broadcast(const QUuid &uuid, ComponentState new_state);

    void _start_offline_timer(const QUuid &uuid);
};

#endif // AGGREGATOR_SERVER_H
