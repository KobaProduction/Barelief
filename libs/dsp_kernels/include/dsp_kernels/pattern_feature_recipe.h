#pragma once
#include <dsp_kernels/spectral_frame_recipe.h>
#include <dsp_kernels/rectangular_filter_bank.h>
#include <dsp_kernels/onset_strength.h>
#include <span>
#include <vector>

namespace dsp_kernels {
// Exact immutable GMMPatternTracker frontend recipe: mono normalized Float32
// PCM at 44100Hz, frame2048/hop882 (50fps), centered normal EOF semantics,
// symmetric unnormalized Hann / raw FFT WITHOUT Nyquist and circular shift;
// 12 bands/octave,30..17000Hz,unnormalized filters; log10(1+x), diff ratio .5,
// positive differences; unnormalized rectangular grouping with crossover270Hz.
// Uses existing Fourier/window/log/filter/difference operations. Only one
// preceding feature row is retained (source lag=1), never a full input history.
// read_frame requires sequential frame indices after reset; PCM and callbacks
// are borrowed for the synchronous call only.
class PreparedPatternFeatureRecipe {
public:
    static SpectralFrameRecipeConfig parameters() noexcept;
    static bool geometry(std::size_t maximum,std::size_t& frames,std::size_t& features) noexcept;
    static bool storage_bytes(std::size_t,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_pcm_samples);
    void reset() noexcept;
    std::size_t frames(std::size_t samples) const noexcept {return spectrum_.frames(samples);}
    std::size_t features() const noexcept {return grouping_.bands();}
    bool read_frame(std::span<const float> pcm,std::size_t frame,std::span<float> output) noexcept;
    bool process(std::span<const float> pcm,FloatMatrixDestination) noexcept;
private:
    bool configure(std::size_t);
    bool bands();
    bool difference() noexcept;
    std::size_t maximum_pcm_=0,next_frame_=0;
    PreparedSpectralFrameRecipe spectrum_;
    PreparedRectangularFilterBank grouping_;
    PreparedOnsetStrength differences_;
    std::vector<float> filtered_,previous_,difference_,grouped_;
};
}
