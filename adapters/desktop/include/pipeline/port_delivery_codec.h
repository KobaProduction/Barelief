#pragma once
#include "pipeline/owned_frame.h"
#include <span>
namespace pipeline::port_delivery {
inline constexpr uint8_t SubscribeId=18,AckId=19,FramesId=20;
inline constexpr uint16_t Version=1,AllTypes=0x1ff;
struct Limits {
    uint32_t max_message_bytes=2*1024*1024;
    uint16_t max_endpoints=2048;
};
enum class Status : uint16_t { Accepted,UnsupportedVersion,UnsupportedType,LimitExceeded,InvalidEndpoint,Busy,Malformed };
struct Result {
    Status status=Status::Accepted;
    size_t required_bytes=0,written_bytes=0;
    explicit operator bool() const {return status==Status::Accepted;}
};
struct Subscribe {
    uint64_t nonce=0;
    Limits limits;
    uint16_t type_mask=AllTypes;
    std::vector<EndpointIdentity> endpoints;
};
struct Ack {
    Status status=Status::Accepted;
    uint64_t nonce=0,session_epoch=0,graph_revision=0;
    Limits limits;
    uint16_t type_mask=AllTypes;
    std::string graph_id,error;
};
struct Frames {
    bool snapshot=false,gap=false;
    uint64_t nonce=0,session_epoch=0,sequence=0;
    DeliveryStamp delivery;
    std::string graph_id;
    std::vector<DeliveredPortFrame> entries;
};
struct PortBinding {
    EndpointIdentity endpoint;domain::NodeKind kind;FrameType type;bool unavailable_only=false;
};
// Control-plane pure admission: COMPLETE exact coverage, delta subset. No state mutations.
bool valid_bootstrap_binding(const PortBinding&) noexcept;
Result validate_admitted_membership(const Frames&,std::span<const PortBinding>) noexcept;
// Desktop DATA boundary. Entire grammar/payload validated before allocating owning
// DTOs. Short destination/malformed inputs leave outputs unchanged.
Result encode(const Subscribe&,std::span<uint8_t>,Limits={});
Result encode(const Ack&,std::span<uint8_t>,Limits={});
Result encode(const Frames&,std::span<uint8_t>,Limits={});
Result decode(std::span<const uint8_t>,Subscribe&,Limits={});
Result decode(std::span<const uint8_t>,Ack&,Limits={});
Result decode(std::span<const uint8_t>,Frames&,Limits={});
}
