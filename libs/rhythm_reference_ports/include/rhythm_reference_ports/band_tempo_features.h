// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
inline constexpr std::array<float,8> default_tempo_band_gains{2,3,2,1,1.2f,2,3,2.5f};
// Source TempoScaleBands (outdated tracker frontend): log compression,
// positive temporal difference, source frame factor and nonlinear scaling.
// frame_time_samples retains the original sample-count parameter, not seconds.
// The piecewise scale is intentionally discontinuous at both thresholds.
// Input is finite nonnegative band power. Failed processing keeps history and
// caller outputs intact. Exact in-place operation is supported.
class PreparedBandTempoFeatures {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::span<const float> gains,float frame_time_samples,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::span<const float> gains=default_tempo_band_gains,float frame_time_samples=512);
    void reset() noexcept;
    bool process(std::span<const float> bands,std::span<float> scaled,float& cumulative) noexcept;
private:
    void allocate(std::span<const float>,float);
    bool differences(float& cumulative) noexcept;
    float frame_factor_=0;
    std::vector<float> gains_,previous_,current_,result_;
};
float tempo_feature_scale(float value,float lower,float upper,float exponent) noexcept;
}
