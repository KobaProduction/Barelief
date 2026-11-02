// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/envelope_tempo.h>
#include <dsp_kernels/beat_tracker.h>
#include <dsp_kernels/event_coordinates.h>
#include <optional>
namespace dsp_kernels {
struct EnvelopeBeatTrackingConfig {
    EnvelopeTempoConfig tempo;
    BeatTrackerConfig tracking;
    bool sparse=true;
    EventCoordinateUnit units=EventCoordinateUnit::Frames;
};
struct BeatTrackingSequenceResult {
    std::span<const float> bpm;
    std::span<const std::uint8_t> dense;
    std::span<const std::size_t> frames;
    std::span<const std::uint64_t> samples;
    std::span<const double> seconds;
};
struct BeatTrackingChannelInput {
    std::span<const float> envelope;
    std::optional<std::span<const float>> bpm;
    std::span<const float> log_prior;
};
struct BeatTrackingDestination {
    void* context=nullptr;
    bool (*write)(void*,std::size_t channel,const BeatTrackingSequenceResult&) noexcept=nullptr;
};
// Complete source beat_track from an explicit envelope. Missing BPM uses the
// shared tempo composition; scalar/per-frame BPM bypasses tempo/prior.
// Offline beat estimates, not confirmed realtime beat events. Results are
// owned borrowed views until next process; channel sinks consume synchronously.
class PreparedEnvelopeBeatTracking {
public:
    static bool storage_bytes(std::size_t,EnvelopeBeatTrackingConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,EnvelopeBeatTrackingConfig={});
    bool process(std::span<const float>,BeatTrackingSequenceResult&,
        std::optional<std::span<const float>> bpm=std::nullopt,std::span<const float> log_prior={}) noexcept;
    bool process_channels(std::span<const BeatTrackingChannelInput>,BeatTrackingDestination) noexcept;
    const PreparedBeatTracker& tracker() const noexcept {return tracker_;}
private:
    bool configure(std::size_t,EnvelopeBeatTrackingConfig);
    void allocate(std::size_t);
    bool select_tempo(std::span<const float>,std::optional<std::span<const float>>,std::span<const float>) noexcept;
    bool process_active(std::span<const float>,BeatTrackingSequenceResult&,
        std::optional<std::span<const float>>,std::span<const float>,bool batch_active) noexcept;
    bool silence(std::span<const float>) noexcept;
    bool result(std::size_t,BeatTrackingSequenceResult&) noexcept;
    bool format_events(std::size_t,BeatTrackingSequenceResult&) noexcept;
    EnvelopeBeatTrackingConfig config_;
    PreparedEnvelopeTempo tempo_;
    PreparedBeatTracker tracker_;
    std::size_t maximum_frames_=0,bpm_count_=0;
    std::vector<float> bpm_;
    std::vector<std::uint8_t> mask_;
    std::vector<std::size_t> events_;
    std::vector<std::uint64_t> samples_;
    std::vector<double> seconds_;
};
}
