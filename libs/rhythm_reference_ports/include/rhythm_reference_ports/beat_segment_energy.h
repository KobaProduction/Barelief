// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/spectral_band_ratios.h>
#include <dsp_kernels/tempogram.h>
#include <array>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
inline constexpr std::array<float,7> default_single_beat_borders{0,200,400,800,1600,3200,22000};
enum class BeatOnsetAlignment {PeakSample,MaximumWindowEnergy};
struct BeatSegmentEnergyConfig {
    float sample_rate=44100,search_seconds=.1f,energy_seconds=.05f;
    BeatOnsetAlignment alignment=BeatOnsetAlignment::PeakSample;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
struct BeatSegmentEnergyResult {float energy=0;std::size_t onset_sample=0;};
// Source SingleBeatLoudness. Source "sumEnergy" actually chooses maximum
// sample power; "peakEnergy" chooses maximum forward-window summed power.
// Strict > preserves the first tie. Duration is truncated then rounded up to
// even; source search+duration minimum input length is retained.
// Explicit Blackman-Harris62/sum(abs)=2, no zero-phase, raw Fourier energy and
// source inclusive band ratios. This spectral energy is not a dB/LUFS measure.
class PreparedBeatSegmentEnergy {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(BeatSegmentEnergyConfig,std::span<const float> borders,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    dsp_kernels::FourierTempogramView spectral_frame() const noexcept {return spectrum_.view();}
    bool prepare(BeatSegmentEnergyConfig={},std::span<const float> borders_hz=default_single_beat_borders,const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> segment,BeatSegmentEnergyResult&,std::span<float> band_ratios) noexcept;
    std::size_t minimum_segment_samples() const noexcept {return search_samples_+energy_samples_;}
    std::size_t bands() const noexcept {return ratios_.size();}
private:
    bool configure(BeatSegmentEnergyConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    bool geometry(BeatSegmentEnergyConfig) noexcept;
    bool prepare_spectrum(const dsp_kernels::FourierBackendFactory*);
    bool align(std::span<const float>,std::size_t&) const noexcept;
    BeatSegmentEnergyConfig config_;
    std::size_t search_samples_=0,energy_samples_=0;
    float effective_rate_=0;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    PreparedSpectralBandRatios bands_;
    std::vector<float> ones_,complex_,magnitude_,ratios_;
};
}
