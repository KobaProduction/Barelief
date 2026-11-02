#pragma once
#include <dsp_kernels/spectral_descriptors.h>
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/complex_flux.h>
#include <dsp_kernels/spectral_alignment.h>
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class SpectralNoveltyMethod {
    HighFrequencyContent,SquaredDifference,PositiveDifference,MaximumDifference,
    PhaseMaskedDifference,ModifiedKullbackLeibler,PhaseDeviation,WeightedPhaseDeviation,
    NormalizedWeightedPhaseDeviation,ComplexDifference,RectifiedComplexDifference,AlignedDifference
};
struct SpectralNoveltyConfig {
    SpectralNoveltyMethod method=SpectralNoveltyMethod::PositiveDifference;
    std::size_t lag=1,frequency_maximum=3,temporal_maximum=3,alignment_radius=1;
    std::ptrdiff_t temporal_origin=0;
    float epsilon=2.220446049250313e-16f;
    bool supplied_differences=false,positive_alignment=false;
};
bool spectral_novelty_uses_phase(SpectralNoveltyMethod) noexcept;
bool spectral_novelty_uses_overlap_lag(SpectralNoveltyMethod) noexcept;
// Source madmom spectral onset functions, decomposed over shared descriptors,
// positive/max differences, phase mask and correlation alignment. Feature/phase
// rows are explicit synchronous borrowed readers. No spectrum inference, FFT,
// preprocessing or complete history storage is hidden here.
// Phase deviation ignores feature filtering; weighted phase/complex operations
// require equal feature/phase shape, as in source. Rectified complex prediction
// uses n-1 while its positive-difference gate uses the explicit configured lag.
// AlignedDifference returns a VECTOR; the other methods return one scalar.
// process_frame is sequential after reset; batch process resets independently.
class PreparedSpectralNovelty {
public:
    static bool storage_bytes(std::size_t frames,std::size_t features,std::size_t phase_bins,
        SpectralNoveltyConfig,std::size_t support_count,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    // Current-batch diagnostics are empty until the relevant operation runs;
    // reset and lag-only batches cannot expose a preceding batch's scratch.
    std::span<const float> difference_values() const noexcept {
        if(config_.method==SpectralNoveltyMethod::PhaseMaskedDifference)
            return next_frame_>config_.lag?phase_mask_.difference_values():std::span<const float>{};
        if(!next_frame_ || (!config_.supplied_differences && next_frame_<=config_.lag))return {};
        return config_.method==SpectralNoveltyMethod::SquaredDifference || config_.method==SpectralNoveltyMethod::PositiveDifference ||
            config_.method==SpectralNoveltyMethod::MaximumDifference?std::span<const float>(differences_):std::span<const float>{};
    }
    std::span<const float> phase_mask_values() const noexcept {
        return config_.method==SpectralNoveltyMethod::PhaseMaskedDifference && next_frame_>config_.lag?
            phase_mask_.phase_mask_values():std::span<const float>{};
    }
    std::span<const float> alignment_correlations() const noexcept {
        return config_.method==SpectralNoveltyMethod::AlignedDifference && next_frame_>config_.lag?
            alignment_.correlations():std::span<const float>{};
    }
    std::ptrdiff_t alignment_offset() const noexcept {
        return config_.method==SpectralNoveltyMethod::AlignedDifference && next_frame_>config_.lag?alignment_.offset():0;
    }
    bool prepare(std::size_t maximum_frames,std::size_t features,std::size_t phase_bins,
                 SpectralNoveltyConfig={},std::span<const FilterBandSupport> supports={});
    void reset() noexcept;
    std::size_t output_features() const noexcept;
    bool process_frame(FloatMatrixSource features,FloatMatrixSource phase,std::size_t frames,
                       std::size_t frame,std::span<float> output) noexcept;
    bool process(FloatMatrixSource features,FloatMatrixSource phase,std::size_t frames,FloatMatrixDestination) noexcept;
private:
    bool configure(std::size_t,std::size_t,std::size_t,SpectralNoveltyConfig,std::span<const FilterBandSupport>);
    bool prepare_operation(std::span<const FilterBandSupport>);
    bool read(FloatMatrixSource,std::size_t,std::span<float>) noexcept;
    bool difference(FloatMatrixSource,std::size_t,float&) noexcept;
    bool phase_descriptor(FloatMatrixSource,FloatMatrixSource,std::size_t,float&) noexcept;
    bool scalar(FloatMatrixSource,FloatMatrixSource,std::size_t,std::size_t,float&) noexcept;
    bool aligned(FloatMatrixSource,std::size_t,std::span<float>) noexcept;
    SpectralNoveltyConfig config_;
    PreparedSpectralDescriptor descriptor_;
    PreparedOnsetStrength difference_;
    PreparedComplexFlux phase_mask_;
    PreparedSpectralAlignment alignment_;
    std::size_t maximum_frames_=0,features_=0,phase_bins_=0,next_frame_=0;
    std::vector<float> current_,previous_,differences_,phase_,unit_magnitude_,ones_,result_;
};
}
