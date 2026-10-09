#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <tuple>
#include <domain/graph_definition.h>
namespace pipeline {
// Endpoint identities and delivery fences shared by owned numeric delivery.
using PortDirection = domain::PortDirection;
struct DeliveryStamp {
    uint64_t graph_revision = 0, epoch = 0;
    bool operator==(const DeliveryStamp&) const = default;
};
// Service epochs span graph applies; revisions order queued work within an epoch.
inline bool delivery_precedes(DeliveryStamp candidate, DeliveryStamp current) noexcept {
    return candidate.epoch<current.epoch ||
        (candidate.epoch==current.epoch && candidate.graph_revision<current.graph_revision);
}
struct EndpointIdentity {
    std::string graph_id;
    int32_t node = -1;
    PortDirection direction = PortDirection::Output;
    uint16_t port = 0;
    friend bool operator==(const EndpointIdentity &, const EndpointIdentity &) = default;
    friend bool operator<(const EndpointIdentity &a, const EndpointIdentity &b) {
        return std::tie(a.graph_id,a.node,a.direction,a.port)<std::tie(b.graph_id,b.node,b.direction,b.port);
    }
};
} // namespace pipeline
