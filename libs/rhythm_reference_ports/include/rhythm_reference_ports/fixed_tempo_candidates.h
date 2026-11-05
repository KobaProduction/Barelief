// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/causal_average.h>
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <rhythm_reference_ports/tempo_distribution_peaks.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct FixedTempoCandidateConfig {
    float sample_rate=44100,minimum_bpm=30,maximum_bpm=560,tolerance_percent=3;
    std::size_t hop_samples=512;
};
enum class FixedTempoCandidateStatus {Unprocessed,Ready,NoPeaks,NoTatum,ThresholdExhausted,Failed};
// Source NoveltyCurveFixedBpmEstimator: whole-curve unbiased ACF, causal mean,
// tatum-derived threshold, repeated histogram/merge of inter-peak BPMs.
// Input length is fixed at cold preparation; offline file recipes prepare for
// their actual curve length. Intermediate ACF and peaks remain observable.
// Source empty-vector access is reported as NoPeaks/NoTatum. Its refinement
// loop otherwise never terminates when <=2 candidates remain; this port stops
// when halving reaches the Float32 fixed point and reports ThresholdExhausted,
// preserving the final candidate set rather than claiming a detected tempo.
class PreparedFixedTempoCandidates {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t frames,FixedTempoCandidateConfig,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t novelty_frames,FixedTempoCandidateConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> novelty) noexcept;
    FixedTempoCandidateStatus status() const noexcept {return status_;}
    std::span<const InterpolatedPeak> candidates() const noexcept {return std::span(result_).first(result_count_);}
    std::span<const float> correlation() const noexcept {return ready_?std::span<const float>(correlation_values_):std::span<const float>{};}
    std::span<const float> smoothed_correlation() const noexcept {return ready_?std::span<const float>(smoothed_):std::span<const float>{};}
    float threshold() const noexcept {return threshold_;}
    std::size_t refinements() const noexcept {return refinements_;}
    std::span<const InterpolatedPeak> initial_peaks() const noexcept {return ready_?initial_.peaks():std::span<const InterpolatedPeak>{};}
    std::span<const float> distribution_histogram() const noexcept {return ready_ && refinements_?distribution_.histogram():std::span<const float>{};}
    std::span<const float> distribution_centers() const noexcept {return ready_ && refinements_?distribution_.centers():std::span<const float>{};}
private:
    void allocate(std::size_t);
    bool threshold_from_local_peaks(float) noexcept;
    bool refine_distribution() noexcept;
    bool configure(std::size_t,FixedTempoCandidateConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_detectors();
    float lag_to_bpm(float) const noexcept;
    float bpm_to_lag(float) const noexcept;
    std::size_t prominent_peaks() noexcept;
    std::size_t period_bpms(std::span<const InterpolatedPeak>,bool strict) noexcept;
    bool tatum(float&) noexcept;
    std::size_t nearest(std::span<const InterpolatedPeak>,float) const noexcept;
    std::size_t local_maximum(std::span<const InterpolatedPeak>,std::size_t,float) const noexcept;
    bool initial_threshold() noexcept;
    bool refine() noexcept;
    bool estimate() noexcept;
    void invalidate() noexcept;
    void copy_candidates() noexcept;
    FixedTempoCandidateConfig config_;
    FixedTempoCandidateStatus status_=FixedTempoCandidateStatus::Unprocessed;
    dsp_kernels::PreparedSpectralAutocorrelation correlation_;
    PreparedCausalAverage average_;
    PreparedInterpolatedPeaks initial_,iteration_;
    PreparedTempoDistributionPeaks distribution_;
    std::size_t frames_=0,minimum_lag_=0,selected_count_=0,result_count_=0,refinements_=0;
    float threshold_=0;
    bool ready_=false;
    std::vector<float> correlation_values_,smoothed_,scratch_;
    std::vector<InterpolatedPeak> selected_;
    std::array<InterpolatedPeak,104> result_{};
};
}
