// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/period_path.h>
#include <rhythm_reference_ports/renewal_beat_decoder.h>
#include <dsp_kernels/prepared_resampler.h>
#include <dsp_kernels/temporal_window.h>
namespace rhythm_reference_ports {
struct PeriodBeatTrackingConfig {
    float frame_rate=44100.f/512.f,min_bpm=40,max_bpm=208;
    unsigned upsampling=1;
    UniformNoisePort noise;
};
// Offline source computation of TempoTapDegara, composed from the independently
// public threshold, temporal frame, period-path and renewal-decoder operations.
// Input is an explicit nonnegative ODF, not PCM. Output mask uses the expanded
// ODF grid: (N-1)*upsampling+1 samples, resolution=1/(frame_rate*upsampling).
// Normalization precedes interpolation; period estimation uses a thresholded
// copy while decoding retains the normalized, unthresholded ODF. The ODF and
// per-period thresholds explicitly share the source FIR length and state.
// All public result views are invalidated before processing, including failures.
// No graph, device, GUI, model weights, or realtime history binding is included.
class PreparedPeriodBeatTracker {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_samples,PeriodBeatTrackingConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_input_samples,PeriodBeatTrackingConfig={});
    std::size_t output_samples(std::size_t input_samples) const noexcept;
    bool process(std::span<const float> onset,std::span<std::uint8_t> beats) noexcept;
    bool beat_times(std::span<float> seconds,std::size_t& count) const noexcept;
    float resolution_seconds() const noexcept {return rate_>0?1.f/rate_:0;}
    std::span<const float> periods_seconds() const noexcept {return std::span(periods_).first(result_blocks_);}
    std::span<const float> block_ends_seconds() const noexcept {return std::span(ends_).first(result_blocks_);}
    std::span<const float> normalized_curve() const noexcept {return std::span(expanded_).first(result_samples_);}
    std::span<const std::uint32_t> state_path() const noexcept {return decoder_.state_path().first(result_samples_);}
private:
    bool configure(std::size_t,PeriodBeatTrackingConfig);
    bool prepare_expansion();
    bool prepare_window();
    bool prepare_estimators();
    bool prepare_threshold(std::size_t maximum);
    void allocate();
    bool normalize_expand(std::span<const float>) noexcept;
    bool estimate_periods(std::size_t samples,std::size_t blocks) noexcept;
    PeriodBeatTrackingConfig config_;
    std::size_t maximum_samples_=0,result_samples_=0,result_blocks_=0;
    float rate_=0;
    dsp_kernels::PreparedTemporalWindow window_;
    dsp_kernels::PreparedResampler expansion_;
    PreparedMovingThreshold threshold_;
    PreparedCausalAverage average_;
    PreparedPeriodPath period_;
    PreparedRenewalBeatDecoder decoder_;
    std::vector<float> normalized_,expanded_,thresholded_,periods_,ends_;
    std::vector<std::uint8_t> mask_;
};
}
