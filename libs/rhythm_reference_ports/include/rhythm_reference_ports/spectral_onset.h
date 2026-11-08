// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/polar_novelty.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class SpectralOnsetMethod {Hfc,Complex,ComplexPhase,Flux,PositiveFlux,Rms};
struct SpectralOnsetConfig {
    SpectralOnsetMethod method=SpectralOnsetMethod::Hfc;
    float sample_rate=44100;
};
// Values-only onset descriptors. Complex and RMS consume linear spectra;
// PositiveFlux consumes explicitly projected/log-compressed values and suppresses
// its first frame. No projection, logarithm, framing or Fourier is performed.
class PreparedSpectralOnset {
public:
    bool prepare(std::size_t bins,SpectralOnsetConfig={});
    static bool storage_bytes(std::size_t bins,SpectralOnsetConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    // Borrowed successful-frame stage: complex distances or supplied PositiveFlux
    // values, otherwise empty. Next process/reset or
    // prepare invalidates the view. Reading it does not run DSP.
    std::span<const float> intermediate_values() const noexcept {return scratch_;}
    void reset() noexcept;
    bool process(std::span<const float> magnitude,std::span<const float> phase,float&) noexcept;
private:
    bool configure(std::size_t,SpectralOnsetConfig);
    bool configure_descriptor(std::size_t,float sample_rate);
    bool prepare_hfc(std::size_t,float sample_rate);
    bool prepare_flux(std::size_t,bool);
    bool polar(std::span<const float>,std::span<const float>,float&) noexcept;
    bool positive_flux(std::span<const float>,float&) noexcept;
    std::size_t bins_=0;
    SpectralOnsetMethod method_=SpectralOnsetMethod::Hfc;
    bool first_=true;
    dsp_kernels::PreparedSpectralDescriptor descriptor_;
    PreparedPolarNovelty polar_;
    PreparedSpectralRmsFlux rms_;
    std::vector<float> scratch_;
};
}
