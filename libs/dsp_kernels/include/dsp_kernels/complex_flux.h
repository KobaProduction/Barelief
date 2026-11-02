#pragma once
#include <dsp_kernels/filter_projection.h>
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/onset_strength.h>
#include <span>
#include <vector>
namespace dsp_kernels {
struct ComplexFluxConfig {
    std::size_t lag=2,frequency_maximum=3,temporal_maximum=3;
    std::ptrdiff_t temporal_origin=0;
    float phase_step=0;
    bool compensated_sum=true;
};
// Source port of madmom complex_flux. Magnitude/log-magnitude bands, FFT phase
// rows and band supports are explicit inputs. phase_step adds an explicit
// per-frequency-bin phase ramp (pi for an even-N circular half-window shift).
// The temporal maximum can look ahead; process_frame is an offline estimate.
class PreparedComplexFlux {
public:
    static bool storage_bytes(std::size_t frames,std::size_t bands,std::size_t bins,
        std::size_t support_count,ComplexFluxConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    std::span<const float> phase_mask_values() const noexcept {return maximum_;}
    std::span<const float> difference_values() const noexcept {return difference_values_;}
    bool prepare(std::size_t maximum_frames,std::size_t bands,std::size_t phase_bins,
                 std::span<const FilterBandSupport> supports={},ComplexFluxConfig={});
    bool process_frame(FloatMatrixSource magnitude,FloatMatrixSource phase,
                       std::size_t frames,std::size_t frame,float& result) noexcept;
private:
    bool configure(std::size_t frames,std::size_t bands,std::size_t bins,
                   std::span<const FilterBandSupport>,ComplexFluxConfig);
    void allocate();
    bool phase_mask(FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool group_delay(FloatMatrixSource,std::size_t) noexcept;
    float band_mask(std::size_t) const noexcept;
    std::size_t reflected(std::ptrdiff_t,std::size_t) const noexcept;
    bool load(FloatMatrixSource,std::size_t,std::span<float>) noexcept;
    std::size_t maximum_frames_=0,bands_=0,phase_bins_=0;
    ComplexFluxConfig config_;
    PreparedOnsetStrength difference_;
    std::vector<FilterBandSupport> supports_;
    std::vector<float> current_,previous_,difference_values_,phase_,delay_,maximum_;
};
}
