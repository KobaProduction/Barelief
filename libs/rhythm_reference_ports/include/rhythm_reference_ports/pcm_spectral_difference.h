#pragma once
#include <dsp_kernels/spectral_frame_recipe.h>
#include <rhythm_reference_ports/spectral_difference_sequence.h>
#include <cstddef>
#include <span>
namespace rhythm_reference_ports {
// Original SuperFluxProcessor preprocessing defaults. Settings are explicit and
// overridable: 24 log bands/octave, no filter normalization, log10(1+x).
dsp_kernels::SpectralFrameRecipeConfig maximum_difference_spectrum_defaults() noexcept;
struct PcmSpectralDifferenceConfig {
    dsp_kernels::SpectralFrameRecipeConfig spectrum=maximum_difference_spectrum_defaults();
    SpectralDifferenceSequenceConfig difference{1,3,true};
    bool automatic_lag=true;
    float overlap_ratio=.5f;
};
// PCM convenience composition around the independently usable spectral frontend
// and stateful difference stage. Source stack_diffs uses the two explicit output
// ports plus shared concat/stack blocks. No separate executor or input retention.
class PreparedPcmSpectralDifference {
public:
    bool prepare(std::size_t maximum_samples,PcmSpectralDifferenceConfig={},
                 std::span<const float> window={},dsp_kernels::ExplicitSpectralFilters={});
    static bool storage_bytes(std::size_t maximum_samples,PcmSpectralDifferenceConfig,std::span<const float>,dsp_kernels::ExplicitSpectralFilters,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return spectrum_.workspace_bytes()+differences_.workspace_bytes();}
    bool process(std::span<const float> pcm,dsp_kernels::FloatMatrixDestination differences,
                 bool reset_state=true,dsp_kernels::FloatMatrixDestination current_features={}) noexcept;
    std::size_t output_frames() const noexcept {return differences_.output_frames();}
    void reset() noexcept {differences_.reset();}
    std::size_t features() const noexcept {return spectrum_.features();}
    const dsp_kernels::PreparedSpectralFrameRecipe& spectrum_stage() const noexcept {return spectrum_;}
private:
    struct Input {PreparedPcmSpectralDifference* self;std::span<const float> pcm;};
    static bool read_features(const void*,std::size_t,std::span<float>) noexcept;
    dsp_kernels::PreparedSpectralFrameRecipe spectrum_;
    PreparedSpectralDifferenceSequence differences_;
    std::size_t maximum_samples_=0;
};
}
