#pragma once
#include <dsp_kernels/tempo_analysis_sequence.h>
#include <dsp_kernels/interval_sequence_search.h>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
struct CrfBeatSequenceConfig {
    TempoAnalysisSequenceConfig tempo;
    float interval_sigma=.18f;
    bool use_factors=false;
    std::size_t num_intervals=5;
};
inline constexpr std::array<float,5> default_beat_interval_factors{.5f,.67f,1.f,1.5f,2.f};
// Source CRFBeatDetectionProcessor: tempo estimation on the original activation,
// source factor/top-N selection, independent truncated smoothing for CRF,
// descending candidate trials, log-likelihood/sequence-length ranking.
// Defaults follow the Python constructor's inherited tempo estimator defaults;
// source CLI overrides (20..240/.09/7) can be passed explicitly through config.
// A no-tempo/undefined-backtrace case yields false + empty events, never a
// fabricated grid. Serial trials retain source ordered-map tie semantics.
class PreparedCrfBeatSequence {
public:
    static bool storage_bytes(std::size_t,CrfBeatSequenceConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,CrfBeatSequenceConfig={},
                 std::span<const float> factors=default_beat_interval_factors);
    bool process(std::span<const float>) noexcept;
    std::span<const float> events() const noexcept {return std::span(events_).first(count_);}
    std::span<const std::size_t> positions() const noexcept {return ready_?search_.positions():std::span<const std::size_t>{};}
    std::span<const std::size_t> trial_intervals() const noexcept {return search_attempted_?search_.trial_intervals():std::span<const std::size_t>{};}
    std::span<const float> normalized_scores() const noexcept {return search_attempted_?search_.normalized_scores():std::span<const float>{};}
    std::span<const HistogramTempo> tempo_candidates() const noexcept {return tempo_ready_?tempo_.candidates():std::span<const HistogramTempo>{};}
private:
    bool configure(std::size_t,CrfBeatSequenceConfig,std::span<const float>);
    bool prepare_numerics(std::size_t,std::size_t);
    bool select_intervals() noexcept;
    bool publish() noexcept;
    CrfBeatSequenceConfig config_;
    PreparedTempoAnalysisSequence tempo_;
    PreparedIntervalSequenceSearch search_;
    std::vector<float> factors_,activation_kernel_,activations_,events_;
    std::vector<std::size_t> intervals_;
    std::size_t maximum_frames_=0,minimum_=0,maximum_=0,count_=0,trials_=0;
    bool ready_=false,tempo_ready_=false,search_attempted_=false;
};
}
