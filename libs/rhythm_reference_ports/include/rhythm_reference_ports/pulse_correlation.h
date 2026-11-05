// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <rhythm_reference_ports/pulse_train_analysis.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Source Percival OSS-window recipe. Its components remain independent APIs:
// generalized ACF -> harmonic enhancement -> interpolated peaks -> pulse score.
// Accepts one explicitly framed OSS vector; audio frontend and Gaussian lag
// consensus are separate blocks. No PCM/FFT metadata is inferred from a frame.
class PreparedPulseCorrelation {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t frames,float frame_rate,float minimum_bpm,float maximum_bpm,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t window_frames,float onset_frame_rate,float minimum_bpm=50,float maximum_bpm=210,const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> onset_window,float& selected_lag) noexcept;
    std::span<const float> correlation() const noexcept {return correlation_values_;}
    std::span<const float> enhanced() const noexcept {return enhanced_;}
    std::span<const InterpolatedPeak> peaks() const noexcept {return peaks_.peaks();}
    std::span<const float> pulse_scores() const noexcept {return scoring_.scores();}
    std::span<const float> pulse_magnitudes() const noexcept {return scoring_.magnitudes();}
    std::span<const float> pulse_variances() const noexcept {return scoring_.variances();}
private:
    bool configure(std::size_t,float,float,float,const dsp_kernels::FourierBackendFactory*);
    dsp_kernels::PreparedSpectralAutocorrelation correlation_;
    PreparedInterpolatedPeaks peaks_;
    PreparedPulseTrainScoring scoring_;
    std::vector<float> correlation_values_,enhanced_,candidate_lags_;
};
}
