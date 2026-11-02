#pragma once
#include <dsp_kernels/metrical_state_space.h>
#include <dsp_kernels/sparse_markov.h>
#include <span>
#include <vector>

namespace dsp_kernels {
// Explicit uniform switch matrix (same pattern=1-p; each other=p/(N-1)).
bool uniform_pattern_switches(std::size_t patterns,float probability,std::vector<float>& output);
// Source BarTransitionModel + MultiPatternTransitionModel. Rules have one
// entry per beat across patterns, each entry controls transitions INTO that beat.
// Empty switch matrix means no pattern change. A supplied matrix follows actual
// source indexing [destination pattern][origin pattern], despite its docstring.
// Source requires identical numbers of nonzero boundary links across patterns.
// No renormalization of supplied switches; CSR preparation checks outgoing sums.
class PreparedMetricalTransitions {
public:
    static bool storage_bytes(std::size_t states,std::size_t patterns,std::size_t beats,std::size_t maximum_periods,
        bool switches,std::size_t& bytes,std::size_t& maximum_edges) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(const PreparedMetricalStateSpace&,std::span<const TempoTransitionRule>,
                 std::span<const float> pattern_switches={});
    std::span<const MarkovTransition> edges() const noexcept {return edges_;}
private:
    bool pattern(const PreparedMetricalStateSpace&,std::size_t,std::span<const TempoTransitionRule>);
    bool boundary(const PreparedMetricalStateSpace&,std::size_t,std::size_t,TempoTransitionRule);
    bool switch_patterns(std::span<const float>);
    bool switches(std::size_t,std::size_t,float);
    std::vector<MarkovTransition> edges_;
    std::vector<std::vector<MarkovTransition>> boundaries_;
    std::vector<std::vector<std::size_t>> boundary_indices_;
};
}
