// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/lag_tempo_vote.h>
#include <rhythm_reference_ports/beat_interval_histogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Source RhythmExtractor2013's tick -> IBI -> nearest-two-BPM vote/mean.
// Reuses the SAME legacy vote with sample_rate=audio_hop=1, so its lag input is
// an interval in seconds. Supplied multifeature confidence is passed through;
// a Degara composition explicitly supplies zero. It is not forced into [0,1].
// Caller-owned ticks are not retained or cloned. Non-increasing ticks, which
// cause source division-by-zero/unsafe histogram indexing, are rejected.
class PreparedBeatIntervalSummary {
public:
    bool prepare(std::size_t maximum_ticks,float maximum_bpm=1000);
    static bool storage_bytes(std::size_t maximum_ticks,float maximum_bpm,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return vote_.workspace_bytes()+intervals_.capacity()*sizeof(float);}
    bool process(std::span<const float> ticks,float supplied_confidence=0) noexcept;
    float bpm() const noexcept {return ready_?vote_.bpm():0;}
    float confidence() const noexcept {return ready_?confidence_:0;}
    std::span<const float> intervals() const noexcept;
    std::span<const float> estimates() const noexcept;
    bool histogram_descriptors(BeatIntervalHistogram&) const noexcept;
private:
    std::size_t maximum_ticks_=0,count_=0;
    float confidence_=0;
    bool ready_=false;
    PreparedLagTempoVote vote_;
    std::vector<float> intervals_;
};
}
