// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/detrended_fluctuation.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct ActivityFluctuationConfig {
    float sample_rate=44100,minimum_tau_ms=310,maximum_tau_ms=8800,tau_multiplier=1.1f;
};
// Source PCM Danceability recipe: complete nonoverlapping 10ms frames,
// sample stddev (n-1), sequential mean subtraction and integration, DFA.
// The remaining partial PCM frame is discarded. Stored activity is the one
// required integrated feature sequence, no full PCM copy. Independent batch.
// Rejects tau<2 / frame<2 and nonadvancing tau generation instead of source
// division-by-zero / infinite loop; valid repeated integer taus are retained
// and may produce an explicit NonFiniteExponent diagnostic in the DFA stage.
class PreparedActivityFluctuation {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool geometry(std::size_t maximum,ActivityFluctuationConfig,std::size_t& frame,std::size_t& scales) noexcept;
    static bool storage_bytes(std::size_t maximum,ActivityFluctuationConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept {return scaling_.workspace_bytes()+activity_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_pcm_samples,ActivityFluctuationConfig={});
    bool process(std::span<const float>) noexcept;
    std::size_t audio_frame_size() const noexcept {return frame_size_;}
    std::span<const float> integrated_activity() const noexcept {return std::span(activity_).first(activity_count_);}
    const PreparedDetrendedFluctuation& scaling() const noexcept {return scaling_;}
private:
    bool configure(std::size_t,ActivityFluctuationConfig);
    bool build_scales(ActivityFluctuationConfig);
    bool integrate(std::span<const float>) noexcept;
    std::size_t maximum_pcm_=0,frame_size_=0,activity_count_=0;
    PreparedDetrendedFluctuation scaling_;
    std::vector<float> activity_;
};
}
