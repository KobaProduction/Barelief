#pragma once
#include <dsp_kernels/pattern_feature_recipe.h>
#include <dsp_kernels/pattern_state_decoder.h>

namespace dsp_kernels {
// Full source GMMPatternTracker numerical composition. Existing spectral recipe
// feeds classical PatternTracking lazily through its synchronous feature reader.
// Fixed44100/50fps source frontend; arbitrary-feature PatternStateDecoder remains
// separately usable. No PCM/feature-history clone, pickle, executor or device I/O.
class PreparedPcmPatternRecipe {
public:
    static bool storage_bytes(std::size_t,std::span<const GmmPatternData>,std::span<const PatternTempoParameters>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return features_.workspace_bytes()+decoder_.workspace_bytes();}
    bool prepare(std::size_t maximum_pcm_samples,std::span<const GmmPatternData>,
                 std::span<const PatternTempoParameters>);
    bool process(std::span<const float> pcm) noexcept;
    const PreparedPatternStateDecoder& result() const noexcept {return decoder_;}
private:
    struct Input {PreparedPcmPatternRecipe* self;std::span<const float> pcm;};
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    std::size_t maximum_pcm_=0;
    PreparedPatternFeatureRecipe features_;
    PreparedPatternStateDecoder decoder_;
};
}
