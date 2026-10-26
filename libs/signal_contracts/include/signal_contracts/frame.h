#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace signal_contracts {
// Stable wire identifiers: 0..6 use BDSP payload v2, Scalar/Packet require v3.
enum class FrameType { PCM=0, Window=1, Windowed=2, StereoSpectrum=3, Spectrum=4, Mel=5, History=6, Scalar=7, WledPacket=8 };
enum class DataShape { Scalar, Vector, Matrix, Packet, Unknown };
enum class Quantity { Unspecified, Amplitude, SpectrumMagnitude, MelEnergy, FilledFrameCount, Packet, SpectrumComplex, SpectrumReal, SpectrumImaginary, SpectrumPhase, SpectrumPeakAmplitude, SpectrumPower, SpectrumPsd, SpectrumAmplitudeDb, SpectrumPowerDb, SpectrumPsdDb, BroadbandEnergy, BroadbandPower, BroadbandRms, BroadbandMaximum, SpectralFlux, NoteSalience, SpectrumRmsAmplitude, SpectrumBinPower, SpectrumWindowScaledMagnitude,
    BroadbandBinPowerSum, BroadbandBinPowerMean, BroadbandBinPowerRms, BroadbandBinPowerMaximum, BroadbandBinPowerFlux };
enum class Unit { Unspecified, NormalizedAmplitude, Frames, Bytes };
struct FrameDescriptor {
    FrameType type;
    std::string_view id;
    DataShape shape;
    bool stereo;
    Quantity quantity;
    Unit unit;
};
std::span<const FrameDescriptor> frame_descriptors() noexcept;
const FrameDescriptor* frame_descriptor(FrameType) noexcept;
constexpr DataShape data_shape(FrameType type) noexcept {
    switch(type) {
    case FrameType::WledPacket:return DataShape::Packet;
    case FrameType::Scalar:return DataShape::Scalar;
    case FrameType::History:return DataShape::Matrix;
    case FrameType::PCM:case FrameType::Window:case FrameType::Windowed:
    case FrameType::StereoSpectrum:case FrameType::Spectrum:case FrameType::Mel:return DataShape::Vector;
    default:return DataShape::Unknown;
    }
}
// UI, domain and adapters share stable type names. A shape alone is never a type.
std::string_view type_id(FrameType) noexcept;
enum class Status { ok, unavailable, invalid_argument, incompatible_contract, buffer_too_small,
                    exhausted, overflow, would_block, disconnected, malformed, unsupported_version };
struct PortContract {
    FrameType type = FrameType::PCM;
    std::size_t sample_rate = 0, fft_size = 0, bands = 0, history_capacity = 0;
    std::size_t columns = 0;
    float frequency_step = 0;
    bool bark_weighted = false;
    friend bool operator==(const PortContract &, const PortContract &) = default;
};
bool compatible(const PortContract &, const PortContract &) noexcept;
struct FrameMetadata {
    std::uint64_t sequence = 0, generation = 0, epoch = 0, graph_revision = 0;
    std::uint64_t source_sequence = 0, producer_epoch = 0;
    std::int64_t timestamp_ns = 0; // Source clock domain; no cross-device clock comparison.
    std::size_t sample_rate = 0, fft_size = 0, bands = 0, history_capacity = 0;
    float frequency_step = 0, peak_hz = 0, magnitude = 0;
    std::string_view stream_id;
    bool discontinuity = false, bark_weighted = false, available = true;
};
struct HistoryView {
    std::span<const float> storage;
    std::size_t bands = 0, frames = 0, capacity = 0, oldest = 0;
    // Optional borrowed replacement of the logical newest row. Physical ring is untouched.
    std::span<const float> newest_override{};
    // Prepared route state, a separate bounded ring. A silenced row stays
    // zero after it ceases to be newest; payload storage remains borrowed/read-only.
    std::span<const std::uint8_t> zero_rows{};
    std::span<const float> zero_row{};
    std::size_t zero_oldest = 0;
    // Logical oldest-to-newest order, stored [frame][band]. Valid after validation.
    std::span<const float> row(std::size_t index) const noexcept {
        const auto slot=(oldest+index)%capacity;
        if(!zero_rows.empty() && zero_rows[(zero_oldest+index)%zero_rows.size()])return zero_row;
        if(!newest_override.empty() && index==frames-1)return newest_override;
        return storage.subspan(slot * bands, bands);
    }
};
// Borrowed data (including stream_id) remains valid only while its owner permits it.
// Runtime output views are valid until the next tick. Queue users must retain them.
struct FrameView {
    FrameType type = FrameType::PCM;
    FrameMetadata metadata;
    std::span<const float> left, right, values;
    HistoryView history;
    std::span<const std::uint8_t> packet;
};
PortContract contract(const FrameView &) noexcept;
// Strict semantic validation; historical codec wire validation remains a separate boundary.
Status validate(const FrameView &) noexcept;
} // namespace signal_contracts
