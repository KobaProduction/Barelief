// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/feature_reduction.h>
#include <dsp_kernels/tempo_estimator.h>
#include <optional>
namespace dsp_kernels {
struct MetricalTempogramSequenceConfig {
    CurveInterpolationConfig interpolation;
    FeatureReductionConfig tempo_reduction{FeatureReduction::Sum,false};
    bool dynamic_frequencies=false;
};
struct MetricalTempogramChannelInput {
    FloatMatrixSource values;std::size_t frames=0;
    FloatMatrixSource frequencies;FloatMatrixReductionPort reduction;
};
// metrogram: explicit [time][tempo-bin] -> interpolated harmonic products.
// Raw rows are [factor][ORIGINAL tempo-bin]; no hidden sorting of output bins.
// Aggregation collapses the tempo axis inside each frame, not the time axis.
// Source NaN/Inf products are retained; they are not evidence of a beat.
class PreparedMetricalTempogramSequence {
public:
    static bool storage_bytes(std::size_t bins,MetricalTempogramSequenceConfig,std::size_t factor_count,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::span<const float> coordinates,MetricalTempogramSequenceConfig={},
        std::optional<std::span<const float>> factors=std::nullopt);
    bool process(FloatMatrixSource,std::size_t frames,FloatMatrixDestination,
        FloatMatrixSource frequencies={},FloatMatrixReductionPort={}) noexcept;
    bool process_channels(std::span<const MetricalTempogramChannelInput>,FloatMatrixBatchDestination) noexcept;
    std::size_t columns() const noexcept {return output_.size();}
    std::size_t frequency_bins() const noexcept {return coordinates_.size();}
    std::size_t factors() const noexcept {return factors_.size();}
private:
    bool configure(std::span<const float>,MetricalTempogramSequenceConfig,std::optional<std::span<const float>>);
    bool evaluate(FloatMatrixSource,FloatMatrixSource,std::size_t,FloatMatrixReductionPort) noexcept;
    bool reduce(FloatMatrixReductionPort) noexcept;
    static bool read_frequency(const void*,std::size_t,std::span<float>) noexcept;
    MetricalTempogramSequenceConfig config_;
    PreparedCurveInterpolation interpolation_;
    std::vector<float> coordinates_,factors_,input_,products_,output_;
    bool prepared_=false;
};
}
