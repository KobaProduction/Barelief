#pragma once
#include <dsp_kernels/metrical_state_space.h>
#include <dsp_kernels/sparse_markov.h>
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {
struct BarStateDecoderConfig {float observation_weight=100,meter_change_probability=1e-7f;};
// Classical DBNBarTracking postprocessor. Explicit beat times and downbeat
// probabilities; the LAST probability is deliberately ignored (NaN permitted).
// Uses common bar/multi-pattern states, transitions and generic sparse HMM.
// Decodes N-1 beats, then extrapolates final beat number using the penultimate
// state's meter. Returned numbers align with caller beat times, no time copy.
// Empty/<2 inputs or impossible paths safely give no numbers instead of source
// path[-1] failure. Source default meters are 3,4; custom switch matrix optional.
class PreparedBarStateDecoder {
public:
    static bool storage_bytes(std::size_t,std::span<const std::uint32_t>,BarStateDecoderConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return space_.workspace_bytes()+model_.workspace_bytes()+numbers_.capacity()*sizeof(std::uint32_t);}
    bool prepare(std::size_t maximum_beats,std::span<const std::uint32_t> beats_per_bar,
                 BarStateDecoderConfig={},std::span<const float> pattern_switches={});
    bool process(std::span<const float> beat_seconds,std::span<const float> downbeat_probabilities) noexcept;
    std::span<const std::uint32_t> beat_numbers() const noexcept {return std::span(numbers_).first(count_);}
    std::span<const std::uint32_t> state_path() const noexcept {return model_.state_path();}
    const PreparedMetricalStateSpace& state_space() const noexcept {return space_;}
private:
    struct Input {std::span<const float> probability;float weight;};
    bool configure(std::size_t,std::span<const std::uint32_t>,BarStateDecoderConfig,std::span<const float>);
    bool prepare_model(std::size_t,std::span<const std::uint32_t>,std::span<const float>);
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    BarStateDecoderConfig config_;
    std::size_t maximum_beats_=0,count_=0;
    PreparedMetricalStateSpace space_;
    PreparedSparseMarkovModel model_;
    std::vector<std::uint32_t> numbers_;
};
}
