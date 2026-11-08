// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <array>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
inline constexpr std::array<float,7> default_single_beat_borders{0,200,400,800,1600,3200,22000};
// Source EnergyBandRatio over adjacent explicit borders. Both endpoints are
// included after normalized-frequency nearest-bin rounding, so adjacent bands
// can overlap by one bin. Borders above Nyquist yield clipped/empty support.
// Total energy reuses SpectralDescriptor; band sums reuse MelFilterBank's
// generic matrix projection, with source Float32 plain squared accumulation.
// Below total energy 1e-10 all ratios are zero. One border means zero bands.
// Source FrequencyBands: adjacent half-open [nearest low,nearest high) bins.
// Unlike EnergyBandRatio, rounding divides by the explicit per-bin Hz step.
// Uses the same generic MelFilterBank matrix projection, no total normalization.
class PreparedFrequencyBandEnergy {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept {return projection_.workspace_bytes();}
    bool prepare(std::size_t bins,float sample_rate,std::span<const float> borders_hz);
    bool process(std::span<const float> magnitude,std::span<float> output) noexcept {return projection_.process(magnitude,output);}
private:
    dsp_kernels::MelFilterBank projection_;
};
class PreparedSpectralBandRatios {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept {return energy_.workspace_bytes()+projection_.workspace_bytes()+result_.capacity()*sizeof(float);}
    bool prepare(std::size_t bins,float sample_rate,std::span<const float> borders_hz);
    bool process(std::span<const float> magnitude,float& total_energy,std::span<float> ratios) noexcept;
    std::size_t bands() const noexcept {return result_.size();}
private:
    bool configure(std::size_t,float,std::span<const float>);
    dsp_kernels::PreparedSpectralDescriptor energy_;
    dsp_kernels::MelFilterBank projection_;
    bool ready_=false;
    std::vector<float> result_;
};
}
