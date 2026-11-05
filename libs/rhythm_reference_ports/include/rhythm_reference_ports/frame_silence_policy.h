// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/uniform_noise.h>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class SilenceBoundaryRecipe {Standard,Streaming};
struct FrameSilenceConfig {
    float threshold_power=1e-6f,noise_trigger_power=1e-10f,noise_gain=1e-10f;
    bool add_noise=true;
    SilenceBoundaryRecipe boundaries=SilenceBoundaryRecipe::Streaming;
    std::uint32_t noise_seed=0;
    UniformNoisePort noise;
};
struct SilenceFrameBounds {std::uint64_t start=0,stop=0,frames=0;};
// Source FrameCutter noise + standard/streaming StartStopSilence states.
// Mean power uses existing plain Float32 squared projection. NoiseAdder really
// multiplies uniform[-1,1] by db2pow(-100)=1e-10 (not amplitude 1e-5).
// Explicit leading-zero/acquired geometry preserves source noise-frame copy:
// copy the FIRST acquired values to offset leading, even for left-padded frames.
// Default seed is explicit/reproducible; a supplied uniform stream allows an
// original-generator oracle. reset() retains random state, as source reset;
// reseed() is explicit. No OS entropy, clock or runtime dependency here.
class PreparedFrameSilencePolicy {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t frame_size,FrameSilenceConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t frame_size,FrameSilenceConfig={});
    void reset() noexcept;
    void reseed(std::uint32_t seed) noexcept {generator_.seed(seed);}
    bool process(std::span<float> frame,std::size_t leading_zeros,std::size_t acquired) noexcept;
    bool finish() noexcept;
    SilenceFrameBounds bounds() const noexcept {return bounds_;}
private:
    bool mean_power(std::span<const float>,float&) noexcept;
    bool add_noise(std::span<float>,std::size_t,std::size_t) noexcept;
    bool observe(bool silent) noexcept;
    void standard(bool silent) noexcept;
    void streaming(bool silent) noexcept;
    FrameSilenceConfig config_;
    std::mt19937 generator_{0};
    SilenceFrameBounds bounds_;
    bool was_silent_=false,finished_=false;
    std::vector<float> ones_,scratch_;
};
}
