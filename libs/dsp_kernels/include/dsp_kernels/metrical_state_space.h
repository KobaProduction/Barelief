#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {
// Source BeatStateSpace's linear/iteratively unique log grid. num_intervals=0
// denotes None. Quantization is nearest-even; all analytical arithmetic Float32.
bool discrete_period_grid(float minimum_interval,float maximum_interval,std::size_t num_intervals,
                           std::vector<std::uint32_t>& output);
// Conservative complete integer-grid geometry, including any source log grid.
bool period_grid_bound(float minimum,float maximum,std::size_t& count,std::size_t& state_sum) noexcept;
struct TempoTransitionRule {float lambda=100;bool fixed_interval=false;};
// Source exponential_transition; None is an index diagonal (fixed_interval).
// Output [from][to], threshold <= is removed, then each row normalized.
bool exponential_period_transitions(std::span<const std::uint32_t> from,std::span<const std::uint32_t> to,
                                    TempoTransitionRule,std::span<float> output,float threshold=2.220446049250313e-16f) noexcept;
struct MetricalPatternConfig {
    std::uint32_t beats=4;
    float minimum_interval=1,maximum_interval=1;
    std::size_t num_intervals=0;
};
// Source BarStateSpace and MultiPatternStateSpace, explicitly stacked patterns.
// State order: [pattern][beat][period][phase]. Position = beat+phase/period;
// periods and pattern identity are explicit tables, no inferred musical clock.
class PreparedMetricalStateSpace {
public:
    static bool storage_bytes(std::span<const MetricalPatternConfig>,std::size_t&) noexcept;
    static bool geometry(std::span<const MetricalPatternConfig>,std::size_t& states,std::size_t& transitions) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::span<const MetricalPatternConfig>);
    std::size_t patterns() const noexcept {return patterns_.size();}
    std::size_t states() const noexcept {return positions_.size();}
    std::size_t beats(std::size_t pattern) const noexcept;
    std::span<const std::uint32_t> periods(std::size_t pattern) const noexcept;
    std::span<const std::uint32_t> first_states(std::size_t pattern,std::size_t beat) const noexcept;
    std::span<const std::uint32_t> last_states(std::size_t pattern,std::size_t beat) const noexcept;
    std::span<const float> positions() const noexcept {return positions_;}
    std::span<const std::uint32_t> state_periods() const noexcept {return state_periods_;}
    std::span<const std::uint32_t> state_patterns() const noexcept {return state_patterns_;}
private:
    struct Pattern {
        std::size_t beats=0;
        std::vector<std::uint32_t> periods,first,last;
    };
    bool append_pattern_beats(Pattern&,std::size_t);
    void reserve_states(std::size_t,std::size_t);
    bool pattern(MetricalPatternConfig,std::size_t);
    bool beat(Pattern&,std::size_t,std::size_t);
    std::vector<Pattern> patterns_;
    std::vector<float> positions_;
    std::vector<std::uint32_t> state_periods_,state_patterns_;
};
}
