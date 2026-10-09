#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wled_protocol {
inline constexpr std::size_t packet_size = 44;
inline constexpr std::size_t bin_count = 16;
inline constexpr std::size_t formatted_mel_count = 50;
inline constexpr std::size_t counter_offset = 17;
using WledPacket = std::array<std::uint8_t, packet_size>;
enum class Status {
    Ok, BufferTooSmall, InvalidSize, LimitExceeded, InvalidHeader,
    NonFiniteInput, InvalidArgument, NonFiniteResult, InvalidField
};
struct Limits { std::size_t max_mel_values; std::size_t max_packet_bytes; };
struct Result { Status status; std::size_t used; std::size_t required; };
struct DecodedPacket {
    std::array<std::uint8_t, 2> pressure;
    float sample_raw, sample_smoothed;
    std::uint8_t sample_peak, counter;
    std::array<std::uint8_t, bin_count> bins;
    std::uint16_t zero_crossing_count;
    float magnitude, peak_hz;
};
// Atomic failure: destination/decoded output unchanged; used=0, required=44.
// Encode accepts >=44 destination bytes; decode/patch require exactly44.
// Legacy bin encoding sanitizes nonfinite/negative signal inputs.
Result encode_bins(std::span<const float> bins, float peak_hz, float magnitude,
    std::uint8_t counter, std::span<std::uint8_t> destination, Limits limits) noexcept;
// Typed Mel: finite nonempty bounded population, gain[0,100], metadata>=0.
Result encode_mel(std::span<const float> mel, float peak_hz, float magnitude,
    float gain, std::uint8_t counter, std::span<std::uint8_t> destination, Limits limits) noexcept;
Result silence(std::uint8_t counter, std::span<std::uint8_t> destination, Limits limits) noexcept;
// Foreign finite floats/raw peak bytes preserved, without producer range clamps.
Result decode(std::span<const std::uint8_t> source, DecodedPacket& output, Limits limits) noexcept;
Status validate_packet(std::span<const std::uint8_t> source, Limits limits) noexcept;
// Only the caller-owned packet's byte17 changes. Counter lifecycle belongs to delivery.
Status write_counter(std::span<std::uint8_t> owned_packet, std::uint8_t counter, Limits limits) noexcept;
namespace legacy {
// Permissive historical entrypoint, separate from typed processing. Numerical
// overflow or an unrepresentable indexing size returns valid signal silence.
WledPacket encode_wled(std::span<const float> mel, float peak_hz, float magnitude,
    float gain, std::uint8_t counter) noexcept;
}
}
