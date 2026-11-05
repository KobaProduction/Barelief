// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/polar_novelty.h>
#include <rhythm_reference_ports/triangular_spectral_bands.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class SpectralOnsetMethod {Hfc,Complex,ComplexPhase,Flux,MelFlux,Rms};
struct SpectralOnsetConfig {
    SpectralOnsetMethod method=SpectralOnsetMethod::Hfc;
    float sample_rate=44100;
};
// Source OnsetDetection six-method composition from explicit linear polar
// spectra. Reuses HFC/flux kernels, polar distances, RMS and Mel projection.
// MelFlux retains 40 HTK/warped unit-sum power bands 0..4000Hz, amp2db and
// first-frame click suppression. Flux is absolute L1; MelFlux is positive L1.
// No window/FFT, source clock, normalization of the ODF or beat decision hidden.
class PreparedSpectralOnset {
public:
    bool prepare(std::size_t bins,SpectralOnsetConfig={});
    static bool storage_bytes(std::size_t bins,SpectralOnsetConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    // Borrowed successful-frame stage: complex distances (bins) or Mel power
    // after source amp2db (40 bands), otherwise empty. Next process/reset or
    // prepare invalidates the view. Reading it does not run DSP.
    std::span<const float> intermediate_values() const noexcept {return scratch_;}
    void reset() noexcept;
    bool process(std::span<const float> magnitude,std::span<const float> phase,float&) noexcept;
private:
    bool configure(std::size_t,SpectralOnsetConfig);
    bool prepare_flux(std::size_t,bool);
    bool prepare_mel(std::size_t,float);
    bool polar(std::span<const float>,std::span<const float>,float&) noexcept;
    bool mel(std::span<const float>,float&) noexcept;
    std::size_t bins_=0;
    SpectralOnsetMethod method_=SpectralOnsetMethod::Hfc;
    bool first_=true;
    dsp_kernels::PreparedSpectralDescriptor descriptor_;
    PreparedPolarNovelty polar_;
    PreparedSpectralRmsFlux rms_;
    PreparedTriangularSpectralBands mel_;
    std::vector<float> scratch_;
};
}
