#pragma once
#include <dsp_kernels/interval_sequence_decoder.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
// CRFBeatDetectionProcessor's factor branch. Integer truncation precedes range
// filtering, as in the source. Values and output use frames, not BPM.
bool factor_beat_intervals(float dominant,std::span<const float> factors,
                          std::size_t minimum,std::size_t maximum,
                          std::span<std::size_t> output,std::size_t& count) noexcept;
// CRF multi-interval search, separately observable from tempo estimation.
// Explicit candidates are sorted descending, decoded independently and ranked
// by log_probability / sequence_length. Equal scores retain the first candidate.
// Input is already smoothed; no tempo/histogram parameters are inferred.
// NoPath trials retain -Inf diagnostics; Error aborts without published results.
class PreparedIntervalSequenceSearch {
public:
    static bool storage_bytes(std::size_t frames,std::size_t low,std::size_t high,std::size_t candidates,float sigma,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return decoder_.workspace_bytes()+(intervals_.capacity()+positions_.capacity())*sizeof(std::size_t)+scores_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_frames,std::size_t minimum_interval,
                 std::size_t maximum_interval,std::size_t maximum_candidates,float sigma=.18f);
    bool process(std::span<const float> activations,std::span<const std::size_t> intervals) noexcept;
    SequenceDecodeOutcome outcome() const noexcept {return outcome_;}
    const char* error_reason() const noexcept {return error_reason_;}
    std::span<const std::size_t> positions() const noexcept {return std::span(positions_).first(count_);}
    std::span<const std::size_t> trial_intervals() const noexcept {return std::span(intervals_).first(trials_);}
    std::span<const float> normalized_scores() const noexcept {return std::span(scores_).first(trials_);}
private:
    void clear_result() noexcept;
    bool fail(const char*) noexcept;
    bool trial(std::span<const float>,std::size_t,float&) noexcept;
    PreparedIntervalSequenceDecoder decoder_;
    std::size_t minimum_=0,maximum_=0,count_=0,trials_=0;
    SequenceDecodeOutcome outcome_=SequenceDecodeOutcome::Error;
    const char* error_reason_="not_processed";
    std::vector<std::size_t> intervals_,positions_;
    std::vector<float> scores_;
};
}
