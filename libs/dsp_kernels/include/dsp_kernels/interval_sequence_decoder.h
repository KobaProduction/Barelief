#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class SequenceDecodeOutcome { Error, NoPath, Path };
// Classical CRF best_sequence from madmom, accepting explicit beat activations
// and an explicit integer interval. No network/model/weights. Full future context,
// N/interval beat variables, log-domain Viterbi and source transition support.
// Only likelihood arithmetic changes from double temporaries to Float32.
// Unreachable/short paths return false with NoPath; malformed inputs/numerics
// return false with Error. A selected finite path must have a consistent trace.
// prepare() reserves the complete worst-case backtrace.
class PreparedIntervalSequenceDecoder {
public:
    static bool storage_bytes(std::size_t frames,std::size_t low,std::size_t high,float sigma,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t minimum_interval,
                 std::size_t maximum_interval,float interval_sigma=.18f);
    bool process(std::span<const float> activations,std::size_t interval) noexcept;
    SequenceDecodeOutcome outcome() const noexcept {return outcome_;}
    const char* error_reason() const noexcept {return error_reason_;}
    std::span<const std::size_t> positions() const noexcept {return std::span(positions_).first(count_);}
    float log_probability() const noexcept {return probability_;}
    std::span<const float> transition_distribution() const noexcept {return std::span(transition_).first(transition_count_);}
private:
    bool decode_sequence(std::size_t,std::size_t) noexcept;
    void clear_result() noexcept;
    bool no_path() noexcept;
    bool fail(const char*) noexcept;
    void allocate();
    bool distribution(std::size_t) noexcept;
    float normalizer(std::span<const float>,std::size_t) const noexcept;
    bool load(std::span<const float>,std::size_t) noexcept;
    void advance(std::size_t frames,std::size_t step) noexcept;
    float predecessor(std::size_t state,std::size_t step) noexcept;
    std::size_t final_state(std::size_t frames) noexcept;
    bool trace(std::size_t frames,std::size_t steps) noexcept;
    std::size_t maximum_frames_=0,minimum_interval_=0,maximum_interval_=0,transition_count_=0,count_=0;
    float sigma_=0,probability_=0;
    SequenceDecodeOutcome outcome_=SequenceDecodeOutcome::Error;
    const char* error_reason_="not_processed";
    std::vector<float> transition_,log_transition_,normalization_,activation_,previous_,current_;
    std::vector<std::size_t> backlinks_,positions_;
};
}
