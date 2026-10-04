#pragma once
#include "aggregator/abstract/types.h"
#include <QByteArray>
#include <QtEndian>
#include <cstring>

// Version 1 preserves the Windows wire layout without relying on packed struct ABI.
namespace aggregator_wire {
inline void append_u16(QByteArray &out, quint16 value) {
    const auto encoded = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}
inline quint16 read_u16(const QByteArray &data, qsizetype offset) {
    return qFromLittleEndian<quint16>(data.constData() + offset);
}
inline QByteArray encode(const registration_request_packet_t &packet) {
    QByteArray out(reinterpret_cast<const char *>(&packet.uuid), 16);
    out.append(static_cast<char>(packet.component_type));
    append_u16(out, packet.component_server_port);
    append_u16(out, packet.session_token);
    return out;
}
inline bool decode(const QByteArray &data, registration_request_packet_t &packet) {
    if (data.size() != 21)
        return false;
    const auto type = static_cast<ComponentType>(static_cast<quint8>(data[16]));
    if (type < ComponentType::data_source || type > ComponentType::core)
        return false;
    std::memcpy(&packet.uuid, data.constData(), 16);
    packet.component_type = type;
    packet.component_server_port = read_u16(data, 17);
    packet.session_token = read_u16(data, 19);
    return true;
}
inline QByteArray encode(const component_update_state_packet_t &packet) {
    QByteArray out(reinterpret_cast<const char *>(&packet.uuid), 16);
    out.append(static_cast<char>(packet.new_state));
    return out;
}
inline bool decode(const QByteArray &data, component_update_state_packet_t &packet) {
    if (data.size() != 17 || static_cast<quint8>(data[16]) > static_cast<quint8>(ComponentState::active))
        return false;
    std::memcpy(&packet.uuid, data.constData(), 16);
    packet.new_state = static_cast<ComponentState>(static_cast<quint8>(data[16]));
    return true;
}
inline QByteArray encode(const component_update_full_packet_t &packet) {
    QByteArray out(reinterpret_cast<const char *>(&packet.uuid), 16);
    out.append(static_cast<char>(packet.type));
    out.append(reinterpret_cast<const char *>(packet.address.c), 16);
    append_u16(out, packet.component_server_port);
    out.append(static_cast<char>(packet.state));
    return out;
}
inline bool decode(const QByteArray &data, component_update_full_packet_t &packet) {
    if (data.size() != 36)
        return false;
    const auto type = static_cast<quint8>(data[16]);
    const auto state = static_cast<quint8>(data[35]);
    if (type < 1 || type > static_cast<quint8>(ComponentType::core) ||
        state > static_cast<quint8>(ComponentState::active))
        return false;
    std::memcpy(&packet.uuid, data.constData(), 16);
    packet.type = static_cast<ComponentType>(type);
    std::memcpy(packet.address.c, data.constData() + 17, 16);
    packet.component_server_port = read_u16(data, 33);
    packet.state = static_cast<ComponentState>(state);
    return true;
}
} // namespace aggregator_wire
