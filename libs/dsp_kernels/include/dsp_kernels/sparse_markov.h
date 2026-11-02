#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace dsp_kernels {
struct MarkovTransition {std::uint32_t from=0,to=0;float probability=0;};
struct SparseMarkovView {
    std::span<const std::size_t> offsets;
    std::span<const std::uint32_t> predecessors;
    std::span<const float> probabilities,log_probabilities;
    std::size_t states() const noexcept {return offsets.empty()?0:offsets.size()-1;}
};
// Incoming CSR, sorted by destination then predecessor. Duplicate edges sum.
// Cold probability validation follows source allclose(sum_outgoing,1).
class PreparedSparseMarkovTransitions {
public:
    static bool storage_bytes(std::size_t states,std::size_t edges,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t states,std::span<const MarkovTransition>);
    SparseMarkovView view() const noexcept {return {offsets_,predecessors_,probabilities_,logs_};}
private:
    bool configure(std::size_t,std::span<const MarkovTransition>);
    bool append(std::span<const MarkovTransition>,std::size_t);
    std::vector<std::size_t> offsets_;
    std::vector<std::uint32_t> predecessors_;
    std::vector<float> probabilities_,logs_;
};
// Shared numerical steps for beat, bar and pattern HMMs. State densities are
// explicit: finite or -inf for Viterbi, finite nonnegative for forward.
// Strict > keeps the first predecessor on ties. Observations are added INSIDE
// the predecessor comparison, retaining source Float32 expression ordering.
bool sparse_viterbi_step(SparseMarkovView,std::span<const float> log_densities,
                         std::span<const float> previous,std::span<float> current,
                         std::span<std::uint32_t> backlinks) noexcept;
bool sparse_forward_step(SparseMarkovView,std::span<const float> densities,
                         std::span<const float> previous,std::span<float> current) noexcept;

enum class MarkovResultStatus {Unprocessed,Ready,NoPath,InvalidInput,ArithmeticFailure};
// Generic classical HMM. No audio/beat assumptions, trained parameters or I/O.
// Observation mapping and initial probabilities are explicit cold inputs.
// viterbi() is independent of forward state, as in source HiddenMarkovModel.
// The synchronous density reader supplies [frame][density] rows, not states.
// -inf paths produce NoPath/empty path; empty input safely produces empty path.
// Prepared backtrace and workspaces; no hot growth or observation-history clone.
class PreparedSparseMarkovModel {
public:
    static bool storage_bytes(std::size_t frames,std::size_t densities,std::size_t states,std::size_t edges,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t density_count,
                 std::span<const std::uint32_t> observation_pointers,
                 std::span<const MarkovTransition>,
                 std::span<const float> initial_probabilities={});
    void reset() noexcept;
    bool viterbi(FloatMatrixSource log_densities,std::size_t frames) noexcept;
    bool forward(FloatMatrixSource densities,std::size_t frames,bool reset_state=true,
                 FloatMatrixDestination posteriors={}) noexcept;
    std::span<const std::uint32_t> state_path() const noexcept {return std::span(path_).first(result_frames_);}
    std::span<const float> forward_probabilities() const noexcept {return forward_previous_;}
    float log_probability() const noexcept {return probability_;}
    MarkovResultStatus status() const noexcept {return status_;}
    SparseMarkovView transitions() const noexcept {return transitions_.view();}
    std::span<const std::uint32_t> observation_pointers() const noexcept {return pointers_;}
private:
    bool configure(std::size_t,std::size_t,std::span<const std::uint32_t>,
                   std::span<const MarkovTransition>,std::span<const float>);
    bool initial(std::span<const float>);
    bool load(FloatMatrixSource,std::size_t,bool logarithmic) noexcept;
    bool trace(std::size_t) noexcept;
    std::size_t maximum_frames_=0,density_count_=0,result_frames_=0;
    float probability_=-std::numeric_limits<float>::infinity();
    MarkovResultStatus status_=MarkovResultStatus::Unprocessed;
    PreparedSparseMarkovTransitions transitions_;
    std::vector<std::uint32_t> pointers_,backlinks_,path_;
    std::vector<float> initial_,row_,state_densities_,previous_,current_,forward_previous_,forward_current_;
};
}
