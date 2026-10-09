#pragma once
#include <algorithm>
#include <wled_protocol/wled_packet.h>

// Public compatibility include; all wire/mapping implementation belongs to
// wled_protocol. Typed callers use its bounded strict APIs directly.
namespace device_protocols {
using WledPacket = wled_protocol::WledPacket;
inline WledPacket wled_packet(std::span<const float> bins, float peak_hz,
    float magnitude, std::uint8_t counter) noexcept {
    WledPacket packet{};
    (void)wled_protocol::encode_bins(bins.first(std::min(bins.size(), wled_protocol::bin_count)),
        peak_hz, magnitude, counter, packet, {0, wled_protocol::packet_size});
    return packet;
}
inline WledPacket encode_wled(std::span<const float> mel, float peak_hz,
    float magnitude, float gain, std::uint8_t counter) noexcept {
    return wled_protocol::legacy::encode_wled(mel, peak_hz, magnitude, gain, counter);
}
}
