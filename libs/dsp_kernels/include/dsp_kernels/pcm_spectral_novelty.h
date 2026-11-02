#pragma once
#include <dsp_kernels/spectral_frame_recipe.h>
#include <dsp_kernels/spectral_novelty.h>
#include <cstddef>
#include <span>
namespace dsp_kernels {
struct PcmSpectralNoveltyConfig {
    SpectralFrameRecipeConfig spectrum;
    SpectralNoveltyConfig novelty;
    bool automatic_difference_lag=true;
    float difference_overlap=.5f;
};
// SpectralOnsetProcessor PCM composition. The enum selects all 11 source ODFs;
// AlignedDifference additionally exposes the source correlation_diff matrix.
// Phase/complex methods force circular shift like the original processor.
// Filtering/logarithm are explicit; defaults are raw unfiltered magnitudes.
// Automatic lag uses the common source window-overlap rule, except MKL and
// correlation_diff whose source default is one. Each batch resets ODF state.
// PCM and row callbacks are synchronous borrowed inputs, never retained.
class PreparedPcmSpectralNovelty {
public:
    static bool storage_bytes(std::size_t maximum,PcmSpectralNoveltyConfig,std::span<const float>,ExplicitSpectralFilters,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return spectrum_.workspace_bytes()+novelty_.workspace_bytes();}
    const PreparedSpectralNovelty& novelty() const noexcept {return novelty_;}
    bool prepare(std::size_t maximum_samples,PcmSpectralNoveltyConfig={},
                 std::span<const float> window={},ExplicitSpectralFilters={});
    bool process(std::span<const float> pcm,FloatMatrixDestination) noexcept;
    std::size_t frames(std::size_t samples) const noexcept {return spectrum_.frames(samples);}
    std::size_t output_features() const noexcept {return novelty_.output_features();}
    const PreparedSpectralFrameRecipe& spectrum() const noexcept {return spectrum_;}
private:
    struct Input {PreparedPcmSpectralNovelty* self;std::span<const float> pcm;};
    static bool read_features(const void*,std::size_t,std::span<float>) noexcept;
    static bool read_phase(const void*,std::size_t,std::span<float>) noexcept;
    PreparedSpectralFrameRecipe spectrum_;
    PreparedSpectralNovelty novelty_;
    std::size_t maximum_samples_=0;
};
}
