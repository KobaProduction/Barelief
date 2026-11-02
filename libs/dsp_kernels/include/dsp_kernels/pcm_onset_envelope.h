// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/pcm_onset_strength.h>
namespace dsp_kernels {
// Shared PCM -> scalar onset-envelope composition for tempo/beat/PLP recipes.
// Only the scalar history required by temporal algorithms is retained.
class PreparedPcmOnsetEnvelope {
public:
    static bool geometry(std::size_t,PcmOnsetStrengthConfig,ExplicitSpectralFilters,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t,PcmOnsetStrengthConfig,std::span<const float>,ExplicitSpectralFilters,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return source_.workspace_bytes()+values_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_samples,PcmOnsetStrengthConfig={},
        std::span<const float> window={},ExplicitSpectralFilters={},OnsetAggregationPort={});
    bool process(std::span<const float>,FloatMatrixSource reference={}) noexcept;
    // Direct caller-owned result for source batch composition; values() is
    // empty after this call, and no borrowed output span is retained.
    bool process_into(std::span<const float>,std::span<float>,FloatMatrixSource reference={}) noexcept;
    std::size_t frames(std::size_t count) const noexcept {return source_.frames(count);}
    std::span<const float> values() const noexcept {return std::span(values_).first(count_);}
private:
    static bool write(void*,std::size_t,std::span<const float>) noexcept;
    static bool write_into(void*,std::size_t,std::span<const float>) noexcept;
    PreparedPcmOnsetStrength source_;
    std::vector<float> values_;
    std::size_t count_=0;
};
}
