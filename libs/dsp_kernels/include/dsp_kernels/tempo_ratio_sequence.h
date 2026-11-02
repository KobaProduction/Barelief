// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/feature_reduction.h>
#include <dsp_kernels/tempo_estimator.h>
#include <optional>
namespace dsp_kernels {
struct TempoRatioSequenceConfig {
    TempoEstimatorConfig tempo;
    CurveInterpolationConfig interpolation;
    FeatureReductionConfig time_reduction;
    float fill=0;
    bool dynamic_frequencies=false;
};
struct TempoRatioChannelInput {
    FloatMatrixSource values;std::size_t frames=0;
    std::optional<std::span<const float>> bpm;
    std::span<const float> log_prior;
    FloatMatrixSource frequencies;
    FloatMatrixReductionPort reduction;
};
// tempogram_ratio from supplied [time][tempo-bin] values. Missing BPM invokes
// the shared per-frame tempo estimator; supplied scalar/per-frame BPM bypasses
// it. Coordinates/factors are explicit, with source defaults when omitted.
// Dynamic coordinates use prepared interpolation capacity, without allocation.
class PreparedTempoRatioSequence {
public:
    static bool storage_bytes(std::size_t frames,std::size_t bins,TempoRatioSequenceConfig,
        std::size_t factor_count,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t bins,TempoRatioSequenceConfig={},
        std::span<const float> coordinates={},
        std::optional<std::span<const float>> factors=std::nullopt);
    bool process(FloatMatrixSource,std::size_t frames,FloatMatrixDestination,
        std::optional<std::span<const float>> bpm=std::nullopt,std::span<const float> log_prior={},
        FloatMatrixSource frequencies={},FloatMatrixReductionPort={}) noexcept;
    bool process_channels(std::span<const TempoRatioChannelInput>,FloatMatrixBatchDestination) noexcept;
    std::size_t output_frames(std::size_t frames) const noexcept {return reduction_.output_rows(frames);}
    std::size_t factors() const noexcept {return factors_.size();}
private:
    struct Rows {PreparedTempoRatioSequence* owner;FloatMatrixSource values,frequencies;std::span<const float> bpm;};
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    bool configure(std::size_t,std::size_t,TempoRatioSequenceConfig,std::span<const float>,
        std::optional<std::span<const float>>);
    bool prepare_axes(std::size_t,TempoRatioSequenceConfig,std::span<const float>,std::optional<std::span<const float>>);
    TempoRatioSequenceConfig config_;
    PreparedTempoEstimator tempo_;
    PreparedCurveInterpolation interpolation_;
    PreparedFeatureReduction reduction_;
    std::size_t maximum_frames_=0,bins_=0;
    std::vector<float> coordinates_,factors_,input_,bpm_;
};
}
