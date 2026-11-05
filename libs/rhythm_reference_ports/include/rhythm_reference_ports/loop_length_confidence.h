// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/envelope_follower.h>
#include <array>
#include <cstdint>
#include <span>

namespace rhythm_reference_ports {
struct LoopLengthConfidenceResult {
    float rounded_bpm=0,confidence=0;
    std::size_t start_sample=0,end_sample=0;
    std::array<std::int64_t,4> durations{};
    std::array<float,4> scores{};
};
// Source LoopBpmConfidence. This is a LOOP-LENGTH fit score, not calibrated
// probability of a correct beat grid. Fresh rectified 10ms/10ms envelope,
// 5%-of-maximum edges, durations {N,N-start,end,end-start}, 1..127 integer beat
// lengths, half-beat distance tolerance. Inclusive end INDEX is intentionally
// not converted to a count. Silence follows the same threshold-zero rule.
// Two deterministic envelope passes avoid retaining/cloning a full PCM/history.
// sample_rate is used as Float32 for durations; its truncated integer is used
// for envelope preparation, matching the original parameter conversion.
class PreparedLoopLengthConfidence {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(float sample_rate,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept {return 0;}
    bool prepare(float sample_rate=44100) noexcept;
    bool process(std::span<const float> pcm,float bpm_estimate,LoopLengthConfidenceResult&) noexcept;
private:
    bool maximum(std::span<const float>,float&) noexcept;
    bool edges(std::span<const float>,float,LoopLengthConfidenceResult&) noexcept;
    bool score(std::size_t,LoopLengthConfidenceResult&) const noexcept;
    float sample_rate_=0;
    dsp_kernels::PreparedEnvelopeFollower envelope_;
};
// Source LoopBpmEstimator's final integer-BPM threshold decision. It does not
// turn the loop-length fit into a generic confidence or run an estimator.
bool select_loop_tempo(float bpm_estimate,float confidence,float threshold,float& bpm) noexcept;
}
