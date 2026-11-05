// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct ContextBeatTrackerConfig {
    std::size_t window=512;
    float frame_rate=44100.f/512.f,preferred_bpm=120,context_variance=3.901f,min_period_frames=25;
};
// Source port of Aubio's context-dependent beat tracker. Input is an explicit
// oldest-first ODF window, called every window/4 ODF frames. Returned offsets
// are relative to the next block, in ODF frames, not PCM samples or seconds.
// This preserves source hypotheses on silence; output is not confirmed truth.
class PreparedContextBeatTracker {
public:
    bool prepare(ContextBeatTrackerConfig={});
    static bool storage_bytes(ContextBeatTrackerConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void reset() noexcept;
    bool process(std::span<const float> odf,std::span<float> offsets,std::size_t& count) noexcept;
    std::size_t step() const noexcept {return rayleigh_.size();}
    std::size_t maximum_beats() const noexcept {return step()+1;}
    float period_frames() const noexcept {return period_;}
    float bpm() const noexcept {return period_>0?60.f*config_.frame_rate/period_:0;}
    unsigned time_signature() const noexcept {return time_signature_;}
    float confidence() const noexcept;
    std::span<const float> autocorrelation() const noexcept {return acf_;}
    std::span<const float> comb_response() const noexcept {return comb_;}
    std::span<const float> phase_response() const noexcept {return phase_response_;}
private:
    bool configure(ContextBeatTrackerConfig);
    void weights() noexcept;
    void comb(unsigned harmonics,bool normalized,std::span<const float> weighting) noexcept;
    bool context_change() noexcept;
    void update_context() noexcept;
    void phase_weights(bool fresh_context) noexcept;
    unsigned meter(float period) const noexcept;
    float lag_value(std::ptrdiff_t index) const noexcept;
    float phase() noexcept;
    bool schedule(float phase,std::size_t& count) noexcept;
    ContextBeatTrackerConfig config_;
    float ray_period_=0,general_=0,context_=0,period_=0,previous_=0,older_=0,last_beat_=0;
    int counter_=0;
    bool step_change_=false;
    unsigned time_signature_=0;
    std::vector<float> rayleigh_,context_weights_,detection_weights_,phase_weights_,reversed_,acf_,comb_,phase_response_,offsets_;
};
}
