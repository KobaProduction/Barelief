#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/curve_interpolation.h>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class TempoAggregation {None,Mean,Median};
struct TempoEstimatorConfig {
    float frame_rate=44100.f/512.f,start_bpm=120,std_bpm=1,max_tempo=320;
    bool limit_max_tempo=true;
    TempoAggregation aggregate=TempoAggregation::Mean;
};
// Source port of librosa feature.tempo. Rows are explicit [time][lag] ACF
// tempogram values, without a hidden spectral frontend. log_prior is either
// empty (original log-normal preference) or one explicit value per lag.
// Aggregation None produces one BPM per frame; Mean/Median produce one BPM.
// A prior-selected tempo on silence is only a candidate, never beat evidence.
// Retains source argmax semantics: an entirely impossible prior can select
// lag zero (+infinity BPM). That value is not a confirmed musical tempo.
class PreparedTempoEstimator {
public:
    static bool storage_bytes(std::size_t frames,std::size_t lags,TempoEstimatorConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t lags,TempoEstimatorConfig={});
    bool process(FloatMatrixSource,std::size_t frames,std::span<float> bpm,
                 std::span<const float> log_prior={},FloatMatrixReductionPort={}) noexcept;
private:
    bool read(FloatMatrixSource,std::size_t) noexcept;
    bool aggregate(FloatMatrixSource,std::size_t) noexcept;
    bool reduce(FloatMatrixSource,std::size_t,FloatMatrixReductionPort) noexcept;
    bool mean(FloatMatrixSource,std::size_t) noexcept;
    bool median(FloatMatrixSource,std::size_t) noexcept;
    std::size_t tempo_cutoff() const noexcept;
    bool select(std::span<const float> prior,float& bpm) const noexcept;
    TempoEstimatorConfig config_;
    std::size_t maximum_frames_=0,lags_=0;
    std::vector<float> row_,aggregate_,correction_,column_,result_;
};
// Source-port primitives for tempogram_ratio/metrogram. Coordinates may be
// monotonic increasing or decreasing; non-finite coordinates (ACF lag zero)
// are ignored. Linear interpolation uses explicit coordinates and fill value.
bool interpolate_tempo_curve(std::span<const float> coordinates,std::span<const float> values,
                             float query,float& result,float fill=0) noexcept;
bool tempogram_ratios(std::span<const float> coordinates,std::span<const float> values,
                      float bpm,std::span<const float> factors,std::span<float> output,float fill=0) noexcept;
bool metrical_tempogram(std::span<const float> coordinates,std::span<const float> values,
                        std::span<const float> factors,std::span<float> output) noexcept;
// Prepared interpolation variants. Caller sets the explicit row with
// curve.set_values() first. Ratio fill is also f0_harmonics' NaN replacement;
// infinities in sampled values saturate to Float32 limits, as in nan_to_num.
// Metrical output is [factor][finite ascending BPM];
// collapsing its tempo axis is a separate shared reduction operation.
bool tempogram_ratios(const PreparedCurveInterpolation& curve,float bpm,
                      std::span<const float> factors,std::span<float> output,float fill=0) noexcept;
bool metrical_tempogram(const PreparedCurveInterpolation& curve,
                        std::span<const float> factors,std::span<float> output) noexcept;
// Source interp_harmonics variant retains the explicit input grid order and
// every source bin. Undefined interpolation products remain NaN/Inf.
bool metrical_tempogram(const PreparedCurveInterpolation&,std::span<const float> coordinates,
    std::span<const float> values,std::span<const float> factors,std::span<float> output) noexcept;
}
