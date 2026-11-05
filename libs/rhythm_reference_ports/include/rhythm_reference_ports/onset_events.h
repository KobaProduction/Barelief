// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/normalized_feature_fusion.h>
#include <rhythm_reference_ports/causal_average.h>
#include <rhythm_reference_ports/adaptive_onset_selection.h>
namespace rhythm_reference_ports {
// Thin source Onsets composition. Individual fusion, FIR and selection stages
// remain independently usable. The smoothing state persists between process()
// calls unless reset explicitly, as in the original algorithm.
class PreparedOnsetEvents {
public:
    bool prepare(std::size_t maximum_features,std::size_t maximum_frames,AdaptiveOnsetSelectionConfig={});
    static bool storage_bytes(std::size_t maximum_features,std::size_t maximum_frames,AdaptiveOnsetSelectionConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return fusion_.workspace_bytes()+average_.workspace_bytes()+selection_.workspace_bytes()+(fused_.capacity()+smoothed_.capacity())*sizeof(float);
    }
    void reset() noexcept;
    bool process(dsp_kernels::FloatMatrixSource,std::size_t features,std::size_t frames,
                 std::span<const float> weights,std::span<std::uint8_t> output,bool reset_filter=false) noexcept;
    bool onset_times(std::span<float> output,std::size_t& count) const noexcept {return selection_.onset_times(output,count);}
    std::span<const float> fused_curve() const noexcept {return std::span(fused_).first(result_frames_);}
    std::span<const float> smoothed_curve() const noexcept {return std::span(smoothed_).first(result_frames_);}
    const PreparedAdaptiveOnsetSelection& selection() const noexcept {return selection_;}
private:
    PreparedNormalizedFeatureFusion fusion_;
    PreparedCausalAverage average_;
    PreparedAdaptiveOnsetSelection selection_;
    std::size_t maximum_frames_=0,result_frames_=0;
    std::vector<float> fused_,smoothed_;
};
}
