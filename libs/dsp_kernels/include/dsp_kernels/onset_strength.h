#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/recursive_filter.h>
#include <cstddef>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class OnsetAggregate { Mean, Median, Sum };
// Shared reduction; Median may reorder the caller's prepared scratch row.
bool aggregate_onset_values(std::span<float>,OnsetAggregate,float& output,bool allow_nonfinite=false) noexcept;
struct OnsetStrengthConfig {
    std::size_t lag=1,max_size=1,n_fft=2048,hop_length=512;
    bool center=true,detrend=false;
    OnsetAggregate aggregate=OnsetAggregate::Mean;
    bool operator==(const OnsetStrengthConfig&) const=default;
};
struct SpectralDifferenceOptions {bool maximum=true,positive=true,zero_infinite=false;};
// Stateless values/reference difference shared by prepared onset recipes and
// fixed-storage observers. Shapes must match; finite inputs are required.
// No maximum filter/history is implicit. Output may alias either input exactly.
// Partial overlap with either input is rejected before output writes.
bool spectral_difference_values(std::span<const float> values,std::span<const float> reference,
                                 std::span<float> output,bool positive=true,bool zero_infinite=false) noexcept;
using OnsetSpectrumRows=FloatMatrixSource;
// Source window-overlap recipe for the spectral-difference lag. Explicit
// coefficients and hop in samples; fractional hop (e.g.220.5) is supported.
// Python's round-to-nearest-even and no-crossing argmax fallback are retained.
bool spectral_difference_lag(std::span<const float> window,float hop_samples,
                             float overlap_ratio,std::size_t& lag_frames) noexcept;
// Port of librosa 1.0.0 onset_strength(_multi), ISC licensed.
// See src/onset_strength.cpp for attribution. S is supplied explicitly,
// normally log-power Mel; no PCM frontend, normalization, peak picking or tempo
// estimator is hidden here. Input layout is [time][feature], oldest first.
// center reproduces librosa's left padding, not an STFT or future lookahead.
// A supplied history is evaluated independently, including detrend's zero state.
class PreparedOnsetStrength {
public:
    bool prepare(std::size_t features,std::size_t maximum_frames,const OnsetStrengthConfig& config={});
    std::size_t output_frames(std::size_t frames) const noexcept;
    bool source_frame_for_output(std::size_t time,std::size_t frames,std::size_t& source) const noexcept {
        return features_ && frames<=maximum_frames_ && time<output_frames(frames) && source_frame(time,frames,source);
    }
    // Explicit reference bypasses maximum filtering when apply_maximum=false.
    bool positive_differences(std::span<const float> current,std::span<const float> previous,
                              std::span<float> output,bool apply_maximum=true) noexcept;
    // Signed/rectified spectral difference over the same prepared max filter.
    // zero_infinite is an explicit source cleanup policy, not NaN suppression.
    bool differences(std::span<const float> current,std::span<const float> previous,
                     std::span<float> output,SpectralDifferenceOptions={}) noexcept;
    bool process(std::span<const float> spectrum,std::size_t frames,std::span<float> output) noexcept;
    bool process(std::span<const float> spectrum,std::span<const float> reference,
                 std::size_t frames,std::span<float> output) noexcept;
    // Synchronous borrowed reader, never retained. Caller validates the complete
    // history; this overload validates every row it actually consumes.
    bool process(OnsetSpectrumRows,std::size_t frames,std::span<float> output) noexcept;
    bool process(OnsetSpectrumRows,OnsetSpectrumRows reference,std::size_t frames,std::span<float> output) noexcept;
    // Source aggregate=False branch: [time][feature] rectified differences,
    // including padding/centering and independent per-feature detrending.
    // Writer is synchronous; a failed writer may have received earlier rows.
    bool process_features(OnsetSpectrumRows,std::size_t frames,FloatMatrixDestination) noexcept;
    bool process_features(OnsetSpectrumRows,OnsetSpectrumRows reference,std::size_t frames,FloatMatrixDestination) noexcept;
    static bool storage_bytes(std::size_t features,std::size_t maximum_frames,const OnsetStrengthConfig&,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
private:
    void filter(std::span<const float>) noexcept;
    float aggregate(std::span<const float>,std::span<const float>,bool apply_maximum) noexcept;
    bool calculate(OnsetSpectrumRows,OnsetSpectrumRows reference,std::size_t frames,std::size_t count,bool apply_maximum) noexcept;
    bool calculate_features(OnsetSpectrumRows,OnsetSpectrumRows reference,std::size_t frames,
                            FloatMatrixDestination,bool apply_maximum) noexcept;
    bool read_pair(OnsetSpectrumRows,OnsetSpectrumRows reference,std::size_t source) noexcept;
    bool source_frame(std::size_t time,std::size_t frames,std::size_t& source) const noexcept;
    bool detrend_features() noexcept;
    bool prepare_detrend();
    std::size_t features_=0,maximum_frames_=0;
    OnsetStrengthConfig config_;
    std::vector<float> reference_,positive_,current_,previous_;
    PreparedRecursiveFilter scalar_detrend_;
    std::vector<PreparedRecursiveFilter> feature_detrend_;
    std::vector<std::size_t> queue_;
    std::vector<float> result_;
};
}
