#include "aggregator/utils.h"

namespace aggregator_utils {
    QString component_state_to_string(ComponentState state) {
        switch (state) {
            case ComponentState::unknown: return "Unknown";
            case ComponentState::offline: return "Offline";
            case ComponentState::disconnected: return "Disconnected";
            case ComponentState::registered: return "Registered";
            case ComponentState::inactive: return "Inactive";
            case ComponentState::idle: return "Idle";
            case ComponentState::active: return "Active";
        }
        return "Unknown";
    }

    QString component_type_to_string(ComponentType type) {
        switch (type) {
            case ComponentType::unknown: return "Unknown";
            case ComponentType::data_source: return "DataSource";
            case ComponentType::data_processor: return "DataProcessor";
            case ComponentType::data_sink: return "DataSink";
            case ComponentType::core: return "Core";
        }
        return "Unknown";
    }

    session_token_t generate_session_token(const QUuid &uuid, ComponentType type, uint16_t component_server_port) {
        // Создаем данные для расчета CRC16
        QByteArray data;
        data.append(uuid.toByteArray());
        data.append(static_cast<char>(type));
        data.append(reinterpret_cast<const char *>(&component_server_port), sizeof(component_server_port));

        // Добавляем случайное число для дополнительной энтропии
        uint32_t random_value = QRandomGenerator::global()->generate();
        data.append(reinterpret_cast<const char *>(&random_value), sizeof(random_value));

        // Вычисляем CRC16 используя современный API
        return qChecksum(QByteArrayView(data));
    }

    QString format_component_info(const QUuid &uuid, ComponentType type, ComponentState state) {
        return QString("UUID: %1, type: %2, state: %3")
                .arg(uuid.toString())
                .arg(component_type_to_string(type))
                .arg(component_state_to_string(state));
    }

    QString format_component_state_change(const QUuid &uuid, ComponentState new_state) {
        return QString("UUID: %1, state: %2")
                .arg(uuid.toString())
                .arg(component_state_to_string(new_state));
    }

    QString format_component_registration(const QUuid &uuid, ComponentType type, const QString &address) {
        return QString("UUID: %1, type: %2, %3")
                .arg(uuid.toString())
                .arg(component_type_to_string(type))
                .arg(address);
    }
} // namespace aggregator_utils
