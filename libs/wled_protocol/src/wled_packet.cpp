#include <wled_protocol/wled_packet.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace wled_protocol {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
Result result(Status status) noexcept { return {status, status == Status::Ok ? packet_size : 0, packet_size}; }
Status encode_boundary(std::span<std::uint8_t> destination, Limits limits) noexcept {
    if (limits.max_packet_bytes < packet_size) return Status::LimitExceeded;
    return destination.size() < packet_size ? Status::BufferTooSmall : Status::Ok;
}
void put_float(WledPacket& bytes, std::size_t offset, float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = std::uint8_t(bits >> (8*i));
}
float get_float(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i) bits |= std::uint32_t(bytes[offset+i]) << (8*i);
    return std::bit_cast<float>(bits);
}
WledPacket make_bins(std::span<const float> bins, float peak_hz, float magnitude, std::uint8_t counter) noexcept {
    WledPacket packet{};
    packet[0] = packet[1] = packet[2] = packet[3] = '0'; packet[4] = '2'; packet[counter_offset] = counter;
    float sum = 0, peak = 0;
    for (std::size_t i = 0; i < bin_count; ++i) {
        const float value = i < bins.size() && std::isfinite(bins[i]) ? std::clamp(bins[i], 0.f, 1.f) : 0.f;
        sum += value; peak = std::max(peak, value); packet[18+i] = std::uint8_t(value * 255);
    }
    put_float(packet, 8, sum / 16 * 255); put_float(packet, 12, sum / 16 * 255);
    packet[16] = peak > .5f;
    put_float(packet, 36, std::isfinite(magnitude) ? std::max(0.f, magnitude) : 0.f);
    put_float(packet, 40, std::isfinite(peak_hz) ? std::max(0.f, peak_hz) : 0.f);
    return packet;
}
// Size bound ensures float coordinates are exactly representable enough to
// clamp/cast to size_t safely, including on 32-bit embedded targets.
constexpr std::size_t max_indexable_mel_values = (std::size_t{1} << 24) - 1;
Status map_mel(std::span<const float> mel, float gain, std::array<float, bin_count>& bins, bool strict) noexcept {
    std::array<float, formatted_mel_count> formatted{};
    if (mel.size() > max_indexable_mel_values) return Status::LimitExceeded;
    for (std::size_t i = 0; i < formatted.size() && !mel.empty(); ++i) {
        const float centre = std::clamp(float(i+1) * float(mel.size()+1) / 51 - 1, 0.f, float(mel.size()-1));
        const auto lo = std::size_t(centre), hi = std::min(lo+1, mel.size()-1);
        const float interpolated = mel[lo] * (1-(centre-lo)) + mel[hi] * (centre-lo);
        formatted[i] = interpolated * gain;
        const bool finite_operands = std::isfinite(mel[lo]) && std::isfinite(mel[hi]) && std::isfinite(gain);
        if ((strict || finite_operands) && (!std::isfinite(interpolated) || !std::isfinite(formatted[i])))
            return Status::NonFiniteResult;
    }
    for (std::size_t i = 0; i < bins.size(); ++i) {
        const int begin = int(float(i)*42.f/16), end = int(float(i+1)*42.f/16);
        for (int j = begin; j < end; ++j) {
            const float value = std::max(0.f, formatted[4+j]);
            const bool finite_operands = std::isfinite(bins[i]) && std::isfinite(value);
            bins[i] += value;
            if (finite_operands && !std::isfinite(bins[i])) return Status::NonFiniteResult;
        }
        if (strict && !std::isfinite(bins[i])) return Status::NonFiniteResult;
        bins[i] /= float(end-begin);
    }
    return Status::Ok;
}
}
Result encode_bins(std::span<const float> bins, float peak_hz, float magnitude,
    std::uint8_t counter, std::span<std::uint8_t> destination, Limits limits) noexcept {
    const auto boundary = encode_boundary(destination, limits);
    if (boundary != Status::Ok) return result(boundary);
    if (bins.size() > bin_count) return result(Status::InvalidSize);
    const auto packet = make_bins(bins, peak_hz, magnitude, counter);
    std::copy(packet.begin(), packet.end(), destination.begin());
    return result(Status::Ok);
}
Result encode_mel(std::span<const float> mel, float peak_hz, float magnitude,
    float gain, std::uint8_t counter, std::span<std::uint8_t> destination, Limits limits) noexcept {
    const auto boundary = encode_boundary(destination, limits);
    if (boundary != Status::Ok) return result(boundary);
    if (mel.empty()) return result(Status::InvalidArgument);
    if (mel.size() > limits.max_mel_values || mel.size() > max_indexable_mel_values) return result(Status::LimitExceeded);
    if (!std::isfinite(peak_hz) || !std::isfinite(magnitude) || !std::isfinite(gain)) return result(Status::NonFiniteInput);
    if (peak_hz < 0 || magnitude < 0 || gain < 0 || gain > 100) return result(Status::InvalidArgument);
    for (float value : mel) if (!std::isfinite(value)) return result(Status::NonFiniteInput);
    std::array<float, bin_count> bins{};
    const auto mapped = map_mel(mel, gain, bins, true);
    if (mapped != Status::Ok) return result(mapped);
    return encode_bins(bins, peak_hz, magnitude, counter, destination, limits);
}
Result silence(std::uint8_t counter, std::span<std::uint8_t> destination, Limits limits) noexcept {
    return encode_bins({}, 0, 0, counter, destination, limits);
}
Result decode(std::span<const std::uint8_t> source, DecodedPacket& output, Limits limits) noexcept {
    if (limits.max_packet_bytes < packet_size) return result(Status::LimitExceeded);
    if (source.size() != packet_size) return result(Status::InvalidSize);
    constexpr std::array<std::uint8_t, 6> header{'0','0','0','0','2',0};
    if (!std::equal(header.begin(), header.end(), source.begin())) return result(Status::InvalidHeader);
    DecodedPacket decoded{};
    decoded.sample_raw = get_float(source, 8); decoded.sample_smoothed = get_float(source, 12);
    decoded.magnitude = get_float(source, 36); decoded.peak_hz = get_float(source, 40);
    if (!std::isfinite(decoded.sample_raw) || !std::isfinite(decoded.sample_smoothed) ||
        !std::isfinite(decoded.magnitude) || !std::isfinite(decoded.peak_hz)) return result(Status::NonFiniteInput);
    std::copy_n(source.begin()+6, 2, decoded.pressure.begin());
    decoded.sample_peak = source[16]; decoded.counter = source[counter_offset];
    std::copy_n(source.begin()+18, bin_count, decoded.bins.begin());
    decoded.zero_crossing_count = std::uint16_t(std::uint16_t(source[34]) | (std::uint16_t(source[35]) << 8));
    output = decoded;
    return result(Status::Ok);
}
Status validate_packet(std::span<const std::uint8_t> source, Limits limits) noexcept {
    DecodedPacket ignored{};
    return decode(source, ignored, limits).status;
}
Status write_counter(std::span<std::uint8_t> owned_packet, std::uint8_t counter, Limits limits) noexcept {
    const auto status = validate_packet(owned_packet, limits);
    if (status == Status::Ok) owned_packet[counter_offset] = counter;
    return status;
}
namespace legacy {
WledPacket encode_wled(std::span<const float> mel, float peak_hz, float magnitude,
    float gain, std::uint8_t counter) noexcept {
    std::array<float, bin_count> bins{};
    if (map_mel(mel, gain, bins, false) != Status::Ok) return make_bins({}, 0, 0, counter);
    return make_bins(bins, peak_hz, magnitude, counter);
}
}
}
