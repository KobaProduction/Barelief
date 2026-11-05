// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/scalar_history.h>
#include <rhythm_reference_ports/reference_window.h>
#include <dsp_kernels/tempogram.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct HopSpectrumConfig {
    std::uint32_t window_samples=1024,hop_samples=512;
    AnalysisWindowShape window=AnalysisWindowShape::PeriodicHann;
};
// Aubio phase-vocoder ANALYSIS stage: zero-initialized raw PCM ring -> window
// -> source fvec_shift (ceil(N/2) left rotation for odd N) -> shared raw RFFT/
// DFT -> explicit source polar conversion, including DC/Nyquist conventions.
// Exactly one complete hop per call, as in aubio_pvoc_do. No new transport,
// resampler, file loader or executor. ScalarHistory avoids copying raw history.
// Window controls preserve PCM history; restart() clears only analysis history.
class PreparedHopSpectrum {
public:
    // Factory is borrowed only during cold prepare/query; prepared copies own
    // independent backend plans. Null preserves the portable default FFT.
    bool prepare(HopSpectrumConfig={},std::span<const float> coefficients={},const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(HopSpectrumConfig,std::span<const float> coefficients,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void restart() noexcept;
    bool process_hop(std::span<const float> pcm,std::span<float> magnitude,std::span<float> phase) noexcept;
    // Separate PCM accumulation from FFT for consumers of raw frame energy.
    bool append_hop(std::span<const float> pcm) noexcept;
    bool analyze_current() noexcept;
    bool set_window(AnalysisWindowShape) noexcept;
    bool set_window_coefficients(std::span<const float>) noexcept;
    std::size_t bins() const noexcept {return spectrum_.bins();}
    std::uint32_t hop_samples() const noexcept {return config_.hop_samples;}
    std::span<const float> complex_bins() const noexcept {return complex_;}
    std::span<const float> raw_pcm_history() const noexcept {return history_.values();}
    std::span<const float> window_coefficients() const noexcept {return spectrum_.window_coefficients();}
    dsp_kernels::FourierTempogramView transform_stage() const noexcept {return spectrum_.view();}
private:
    bool configure(HopSpectrumConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    HopSpectrumConfig config_;
    ScalarHistory history_;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    std::vector<float> complex_,coefficients_;
};
}
