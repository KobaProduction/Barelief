// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/recursive_filter.h>
namespace dsp_kernels {
struct OnsetFeatureGroup {
    std::span<const std::size_t> indices; // Resolved source slices/boundaries.
};
struct OnsetAggregationPort {
    void* context=nullptr;
    bool (*reduce)(void*,std::span<const float>,float&) noexcept=nullptr;
};
struct OnsetStrengthSequenceConfig {
    OnsetStrengthConfig strength;
    bool aggregate_features=true;
};
// onset_strength_multi composition: shared rectified differences -> explicit
// feature groups -> aggregation -> independent detrending per output.
// No aggregation returns every feature and ignores groups, as in the source.
// Indices/windows are copied cold; supplied reducer lifetime exceeds the plan.
// Rows and writer are synchronous borrowed callbacks, never retained.
class PreparedOnsetStrengthSequence {
public:
    static bool storage_bytes(std::size_t features,std::size_t maximum_frames,OnsetStrengthSequenceConfig,
        std::span<const OnsetFeatureGroup>,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t features,std::size_t maximum_frames,OnsetStrengthSequenceConfig,
        std::size_t group_count,std::size_t index_count,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t features,std::size_t maximum_frames,OnsetStrengthSequenceConfig={},
        std::span<const OnsetFeatureGroup> groups={},OnsetAggregationPort={});
    std::size_t output_frames(std::size_t count) const noexcept {return difference_.output_frames(count);}
    std::size_t output_features() const noexcept {return config_.aggregate_features?groups_.size():features_;}
    bool process(FloatMatrixSource,std::size_t frames,FloatMatrixDestination,FloatMatrixSource reference={}) noexcept;
private:
    struct Writer {PreparedOnsetStrengthSequence* owner;FloatMatrixDestination output;std::size_t frames;};
    static bool write(void*,std::size_t,std::span<const float>) noexcept;
    bool configure(std::size_t,std::size_t,OnsetStrengthSequenceConfig,std::span<const OnsetFeatureGroup>,OnsetAggregationPort);
    bool prepare_groups(std::span<const OnsetFeatureGroup>);
    bool reduce_groups(std::span<const float>,bool active) noexcept;
    PreparedOnsetStrength difference_;
    OnsetStrengthSequenceConfig config_;
    OnsetAggregationPort reducer_;
    std::size_t features_=0;
    std::vector<std::vector<std::size_t>> groups_;
    std::vector<PreparedRecursiveFilter> detrend_;
    std::vector<float> scratch_,output_;
};
}
