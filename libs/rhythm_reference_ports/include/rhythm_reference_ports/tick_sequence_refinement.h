// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct TickSequenceRefinementConfig {
    float frame_rate=44100.f/256.f,minimum_interval_seconds=.24f,last_beat_interval_seconds=.1f;
};
enum class TickSequenceStatus {Unprocessed,Ready,InvalidInput,CapacityExceeded,NonAdvancingExtension};
// Source legacy RhythmExtractor's final ticks/IBIs. Explicit frame count and
// silence boundaries, in feature frames. Retains the source five-second
// stop-silence floor, end extension BEFORE pruning, strict tolerance, backward
// off-beat removal, and the single-tick exception (no pruning when size<=1).
// No sorting, quantization, hidden silence detection or input history retention.
class PreparedTickSequenceRefinement {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_ticks,TickSequenceRefinementConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_ticks,TickSequenceRefinementConfig={});
    bool process(std::span<const float> candidates,std::uint64_t total_frames,
                 float start_silence_frame,float stop_silence_frame) noexcept;
    std::span<const float> ticks() const noexcept;
    std::span<const float> intervals() const noexcept;
    TickSequenceStatus status() const noexcept {return status_;}
private:
    bool extend(float file_length) noexcept;
    void clip(float begin,float end) noexcept;
    void separate() noexcept;
    bool backward_offbeat(std::size_t) const noexcept;
    void remove_offbeats() noexcept;
    bool finalize() noexcept;
    TickSequenceRefinementConfig config_;
    std::size_t count_=0;
    TickSequenceStatus status_=TickSequenceStatus::Unprocessed;
    std::vector<float> ticks_,intervals_;
};
}
