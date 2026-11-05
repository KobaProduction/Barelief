// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/hop_spectrum.h>
#include <rhythm_reference_ports/onset_recipes.h>
#include <rhythm_reference_ports/detail/predictive_reference_frontend.h>
#include <dsp_kernels/numerical_precision.h>
namespace rhythm_reference_ports {
struct PcmPredictiveOnsetConfig {
    std::uint32_t hop_samples=512,frame_samples=1024;
    PredictiveOnsetMethod method=PredictiveOnsetMethod::PositiveComplexDifference;
    PredictiveWindowShape window=PredictiveWindowShape::Hann;
    dsp_kernels::NumericalPrecision precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
// Complete BTrack ODF frontend. Energy uses the raw overlapping PCM frame;
// spectral modes share the existing FFT, full-spectrum expansion and descriptor.
// Warm method changes retain the source's separate magnitude/phase/energy state.
class PreparedPcmPredictiveOnset {
public:
    // DesktopReference preserves all ten source ODFs/five windows with an
    // explicit Float32 complex FFT factory and even N. No factory is retained.
    bool prepare(PcmPredictiveOnsetConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(PcmPredictiveOnsetConfig,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool process_hop(std::span<const float>,float&) noexcept;
    bool set_method(PredictiveOnsetMethod) noexcept;
    void restart() noexcept;
    const PcmPredictiveOnsetConfig& config() const noexcept {return config_;}
    std::span<const float> magnitude() const noexcept {return magnitude_;}
    std::span<const float> phase() const noexcept {return phase_;}
    std::span<const float> raw_pcm_history() const noexcept;
    std::span<const float> complex_values() const noexcept {return complex_;}
    const PreparedHopSpectrum& spectrum_stage() const noexcept {return spectrum_;}
    // Source-profile diagnostics: empty in PortableFloat32. spectrum_stage()
    // belongs to the PortableFloat32 route and is unprepared in this profile.
    const detail::PreparedPredictiveReferenceFrontend& reference_stage() const noexcept {return reference_;}
private:
    bool configure_reference(PcmPredictiveOnsetConfig,const dsp_kernels::FourierBackendFactory*);
    bool configure(PcmPredictiveOnsetConfig,const dsp_kernels::FourierBackendFactory*);
    bool configure_analysis(PcmPredictiveOnsetConfig);
    bool process_spectral(float&) noexcept;
    PcmPredictiveOnsetConfig config_;
    PreparedHopSpectrum spectrum_;
    detail::PreparedPredictiveReferenceFrontend reference_;
    dsp_kernels::PreparedSpectralDescriptor descriptor_;
    dsp_kernels::PreparedEnergyNovelty energy_,energy_difference_;
    std::vector<float> complex_,magnitude_,phase_;
    bool prepared_=false;
};
}
