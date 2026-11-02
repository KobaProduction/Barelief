// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/onset_peaks.h>
#include <dsp_kernels/event_coordinates.h>
#include <optional>
namespace dsp_kernels {
struct OnsetDetectionSequenceConfig {
    float sample_rate=44100;
    std::size_t hop_samples=512;
    bool sparse=true,backtrack=false;
    EventCoordinateUnit units=EventCoordinateUnit::Frames;
    std::optional<OnsetPeakConfig> peaks; // Empty selects source rate-dependent defaults.
};
struct OnsetDetectionResult {
    std::span<const std::uint8_t> dense;
    std::span<const std::size_t> frames;
    std::span<const std::uint64_t> samples;
    std::span<const double> seconds;
};
struct OnsetDetectionDestination {
    void* context=nullptr;
    bool (*write)(void*,std::size_t channel,const OnsetDetectionResult&) noexcept=nullptr;
};
// onset_detect composition from an explicit envelope. Each call is a complete
// independent source window. Sparse backtracking preserves repeated event
// indices; dense mode returns a mask and ignores units. Nonfinite envelopes
// produce no events, matching source onset_detect rather than raw peak_pick.
// Results are borrowed until the next process; channel sinks consume them
// synchronously. Dense multichannel processing keeps per-channel normalization,
// but any nonfinite normalized channel suppresses the whole batch, as in source.
class PreparedOnsetDetectionSequence {
public:
    static bool storage_bytes(std::size_t,OnsetDetectionSequenceConfig,std::size_t energy_frames,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,OnsetDetectionSequenceConfig={},std::size_t maximum_energy_frames=0);
    bool process(std::span<const float> envelope,OnsetDetectionResult&,
        std::optional<std::span<const float>> energy=std::nullopt) noexcept;
    bool process_channels(std::span<const std::span<const float>>,OnsetDetectionDestination) noexcept;
private:
    bool configure(std::size_t,OnsetDetectionSequenceConfig,std::size_t);
    bool select(std::span<const float>,std::optional<std::span<const float>>,std::size_t&,bool batch_active) noexcept;
    bool process_active(std::span<const float>,OnsetDetectionResult&,
                        std::optional<std::span<const float>>,bool batch_active) noexcept;
    bool result(std::size_t frames,std::size_t events,OnsetDetectionResult&) noexcept;
    bool empty_result(std::size_t frames,OnsetDetectionResult&) noexcept;
    bool batch_suppressed(std::span<const std::span<const float>>,bool&) const noexcept;
    OnsetDetectionSequenceConfig config_;
    PreparedOnsetPeakPicker picker_;
    PreparedOnsetBacktracker backtracker_;
    bool normalize_=true;
    std::size_t maximum_frames_=0;
    std::vector<std::uint8_t> mask_;
    std::vector<std::size_t> events_,backtracked_;
    std::vector<std::uint64_t> samples_;
    std::vector<double> seconds_;
};
}
