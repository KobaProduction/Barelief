#pragma once
#include <span>

namespace dsp_kernels {
struct EnvelopeFollowerConfig {
    float sample_rate=44100,attack_seconds=.01f,release_seconds=1.5f;
    bool rectify=true,flush_subnormal_state=true;
};
// Asymmetric first-order smoothing, y=(1-g)*x+g*previous, with explicit
// attack/release time constants. Output is published before optional subnormal
// state cleanup. Scalar processing is atomic. A vector consumes its successful
// prefix on arithmetic overflow; exact in-place buffers are supported.
// reset_before defaults to an independent envelope for each input vector.
class PreparedEnvelopeFollower {
public:
    bool prepare(EnvelopeFollowerConfig={}) noexcept;
    void reset() noexcept {state_=0;}
    bool process(float input,float& output) noexcept;
    bool process(std::span<const float>,std::span<float>,bool reset_before=true) noexcept;
private:
    float attack_=0,release_=0,state_=0;
    bool ready_=false,rectify_=true,flush_subnormal_=true;
};
}
