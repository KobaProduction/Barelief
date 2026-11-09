#pragma once
#include <dsp_kernels/gaussian_mixture.h>
#include <dsp_kernels/metrical_state_space.h>
#include <dsp_kernels/sparse_markov.h>
#include <span>
#include <vector>

namespace dsp_kernels {
struct GmmPatternData {
    std::uint32_t beats=0;
    std::vector<GaussianMixtureData> gmms;
};
struct PatternTempoParameters {
    float minimum_bpm=55,maximum_bpm=205;
    std::size_t num_tempi=0;
    TempoTransitionRule transition;
    // Optional explicit source BarTransitionModel list, one rule INTO each beat.
    std::vector<TempoTransitionRule> beat_transitions;
};
// Full source classical PatternTracking from supplied multiband feature rows.
// All patterns are stacked into ONE HMM with uniform per-state prior, without
// pattern switches; not a collection of independently normalized model scores.
// Source GMM observation profiles are assigned uniformly over each pattern's
// bar positions. Uses existing metrical states/transitions, GMM and sparse HMM.
// Tempo parameters: one record broadcast to all patterns, or one per pattern.
// Pattern data are copied/prepared cold; feature readers are synchronous only.
// Emits beat-number CHANGES, preserving the original missing first/last beats.
class PreparedPatternStateDecoder {
public:
    static bool storage_bytes(std::size_t,std::span<const GmmPatternData>,std::span<const PatternTempoParameters>,float,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::span<const GmmPatternData>,
                 std::span<const PatternTempoParameters>,float frame_rate=50);
    bool process(FloatMatrixSource features,std::size_t frames) noexcept;
    std::size_t feature_count() const noexcept {return features_;}
    std::span<const float> beat_times() const noexcept {return std::span(times_).first(count_);}
    std::span<const std::uint32_t> beat_numbers() const noexcept {return std::span(numbers_).first(count_);}
    std::span<const std::uint32_t> state_path() const noexcept {return model_.state_path();}
    float log_probability() const noexcept {return model_.log_probability();}
    const PreparedMetricalStateSpace& state_space() const noexcept {return space_;}
private:
    bool reserve_rules(std::span<const GmmPatternData>);
    bool append_pattern_mixtures(const GmmPatternData&);
    void append_rules(std::uint32_t,const PatternTempoParameters&);
    bool append_mixture(const GaussianMixtureData&);
    struct Input {PreparedPatternStateDecoder* self;FloatMatrixSource features;};
    bool configure(std::size_t,std::span<const GmmPatternData>,std::span<const PatternTempoParameters>,float);
    bool states(std::span<const GmmPatternData>,std::span<const PatternTempoParameters>);
    bool mixtures(std::span<const GmmPatternData>);
    bool model(std::size_t);
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    std::size_t maximum_frames_=0,features_=0,count_=0;
    float rate_=0;
    PreparedMetricalStateSpace space_;
    PreparedSparseMarkovModel model_;
    std::vector<TempoTransitionRule> rules_;
    std::vector<std::size_t> profile_offsets_;
    std::vector<PreparedGaussianMixture> gmms_;
    std::vector<float> row_,times_;
    std::vector<std::uint32_t> frame_indices_,numbers_;
};
}
