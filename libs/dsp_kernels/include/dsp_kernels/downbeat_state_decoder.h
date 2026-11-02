#pragma once
#include <dsp_kernels/metrical_state_space.h>
#include <dsp_kernels/sparse_markov.h>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace dsp_kernels {
struct DownbeatPatternConfig {
    std::uint32_t beats=4;
    float minimum_bpm=55,maximum_bpm=215,transition_lambda=100;
    std::size_t num_tempi=60;
    bool fixed_interval=false;
};
struct DownbeatStateDecoderConfig {
    float frame_rate=100,observation_lambda=16,activation_threshold=.05f;
    bool correct=true;
};
// Classical DBNDownBeatTracking from explicit [frame][beat,downbeat] probabilities.
// Each meter has its own BarStateSpace/HMM; select highest whole-path likelihood,
// FIRST model on ties. This processor does not switch meter inside a sequence;
// beat-timed PreparedBarStateDecoder is the separate source multi-pattern recipe.
// Threshold crop retains source idx.any() behavior (only frame0 -> empty).
// correct=true picks flattened two-channel argmax in each beat-state region;
// false returns beat-number changes, preserving missed first/last source beats.
class PreparedDownbeatStateDecoder {
public:
    static bool storage_bytes(std::size_t,std::span<const DownbeatPatternConfig>,DownbeatStateDecoderConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::span<const DownbeatPatternConfig>,DownbeatStateDecoderConfig={});
    bool process(std::span<const float> beat_downbeat_activations) noexcept;
    std::span<const float> beat_times() const noexcept {return std::span(times_).first(count_);}
    std::span<const std::uint32_t> beat_numbers() const noexcept {return std::span(numbers_).first(count_);}
    std::span<const float> model_likelihoods() const noexcept {return likelihoods_;}
    std::span<const std::uint32_t> state_path() const noexcept;
    std::size_t selected_model() const noexcept {return selected_;}
    std::size_t state_path_first_frame() const noexcept {return first_;}
private:
    struct Input {std::span<const float> activations;float lambda;};
    bool configure(std::size_t,std::span<const DownbeatPatternConfig>,DownbeatStateDecoderConfig);
    bool pattern(std::size_t,const DownbeatPatternConfig&);
    bool valid(std::span<const float>) const noexcept;
    void crop(std::span<const float>,std::size_t&,std::size_t&) const noexcept;
    bool decode(std::span<const float>) noexcept;
    bool emit(std::size_t,std::span<const std::uint32_t>) noexcept;
    bool corrected(std::span<const float>,std::span<const std::uint32_t>) noexcept;
    bool changed(std::span<const std::uint32_t>) noexcept;
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    DownbeatStateDecoderConfig config_;
    std::size_t maximum_frames_=0,selected_=std::numeric_limits<std::size_t>::max(),first_=0,count_=0;
    std::vector<PreparedMetricalStateSpace> spaces_;
    std::vector<PreparedSparseMarkovModel> models_;
    std::vector<float> likelihoods_,times_;
    std::vector<std::uint32_t> numbers_,frame_indices_;
};
}
