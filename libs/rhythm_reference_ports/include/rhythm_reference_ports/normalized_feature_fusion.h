// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
// Onsets' feature fusion, independently callable. Source rows are [feature][time].
// Each whole row is divided by its signed maximum (zero maximum is bypassed),
// then summed in feature order with explicit weights. No smoothing or clipping.
class PreparedNormalizedFeatureFusion {
public:
    bool prepare(std::size_t maximum_features,std::size_t maximum_frames);
    static bool storage_bytes(std::size_t maximum_features,std::size_t maximum_frames,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return (row_.capacity()+result_.capacity()+maxima_.capacity())*sizeof(float);}
    void reset() noexcept {result_features_=0;}
    bool process(dsp_kernels::FloatMatrixSource,std::size_t features,std::size_t frames,
                 std::span<const float> weights,std::span<float> output) noexcept;
    std::span<const float> maxima() const noexcept {return std::span(maxima_).first(result_features_);}
private:
    bool accumulate(dsp_kernels::FloatMatrixSource,std::size_t feature,std::size_t frames,float weight) noexcept;
    std::size_t maximum_features_=0,maximum_frames_=0,result_features_=0;
    std::vector<float> row_,result_,maxima_;
};
}
