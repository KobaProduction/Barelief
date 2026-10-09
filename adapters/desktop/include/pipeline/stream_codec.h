#pragma once
#include "pipeline/typed_frame.h"
#include <span>
#include <limits>
#include <algorithm>
#include <bdsp_protocol/stream_codec.h>
namespace pipeline::stream {
using Frame = TypedFrame;
using Type = FrameType;
constexpr size_t MaxFrameBytes = 2 * 1024 * 1024;
inline constexpr size_t InvalidByteBound=std::numeric_limits<size_t>::max();
// Pinned BDSP raw <=q+4, COBSZPE depth224: code/223 + final code/delimiter.
constexpr size_t framed_packet_bound(size_t q) noexcept {return q<=65535?q+6+(q+4)/223:InvalidByteBound;}
constexpr size_t fragmented_message_bound(size_t bytes,size_t envelope,bool direct=false) noexcept {
    if(bytes>MaxFrameBytes || envelope>65535-60000)return InvalidByteBound;
    if(direct && bytes<=65535)return framed_packet_bound(bytes);
    size_t total=0;for(size_t off=0;off<bytes;) {const auto chunk=std::min(size_t(60000),bytes-off);const auto bound=framed_packet_bound(chunk+envelope);if(bound>InvalidByteBound-total)return InvalidByteBound;total+=bound;off+=chunk;}
    return total;
}
constexpr bool peer_budget_fits(size_t socket_bytes,size_t channel_bytes,size_t assembly_bytes,size_t next_bytes) noexcept {
    size_t room=MaxFrameBytes;for(const auto value:{socket_bytes,channel_bytes,assembly_bytes,next_bytes}){if(value>room)return false;room-=value;}return true;
}
using bdsp_protocol::CodecResult;
using bdsp_protocol::encode;
using bdsp_protocol::decode;
bool valid(const Frame &frame);
size_t payload_size(const Frame &frame);
std::vector<uint8_t> encode(const Frame &frame);
bool decode(std::span<const uint8_t> bytes, Frame &frame, std::string &error);
} // namespace pipeline::stream
