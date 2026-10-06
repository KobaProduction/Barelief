#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace signal_contracts {
// Stable values are also the BDSP v2 wire type identifiers.
enum class FrameType { PCM, Window, Windowed, StereoSpectrum, Spectrum, Mel, History };
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
    // Logical oldest-to-newest order, stored [frame][band]. Valid after validation.
    std::span<const float> row(std::size_t index) const noexcept {
        if(!newest_override.empty() && index==frames-1)return newest_override;
        return storage.subspan(((oldest + index) % capacity) * bands, bands);
    }
};
// Borrowed data (including stream_id) remains valid only while its owner permits it.
// Runtime output views are valid until the next tick. Queue users must retain them.
struct FrameView {
    FrameType type = FrameType::PCM;
    FrameMetadata metadata;
    std::span<const float> left, right, values;
    HistoryView history;
};
PortContract contract(const FrameView &) noexcept;
// Strict semantic validation; historical codec wire validation remains a separate boundary.
Status validate(const FrameView &) noexcept;
} // namespace signal_contracts
