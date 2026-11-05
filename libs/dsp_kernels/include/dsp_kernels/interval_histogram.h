#pragma once
#include <dsp_kernels/comb_filter_bank.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class IntervalHistogramMethod {Autocorrelation,Comb};
struct IntervalHistogramConfig {
    std::size_t minimum_interval=24,maximum_interval=150,history_frames=1000;
    IntervalHistogramMethod method=IntervalHistogramMethod::Comb;
    float alpha=.79f;
    std::size_t channels=1;
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
// Source floor/ceil interval limits and truncated online history duration.
bool interval_histogram_parameters(float frame_rate,float minimum_bpm,float maximum_bpm,
                                   float history_seconds,IntervalHistogramConfig&) noexcept;
// madmom tempo.py ACF/comb interval histograms. ACF is mono; comb accepts
// explicit [time][channel] input and returns bins in [channel][interval] order.
// Offline ACF sums abs(products); online ACF retains signed products, as in the
// source. Comb histogram retains every tied maximum at each frame. Offline uses
// the entire input; online uses the prepared rolling history. Smoothing and
// histogram peak extraction are separate operations.
// Preparation copies one alpha or one per interval; empty uses config.alpha.
// Processing spans contain interleaved time-major frames, including online.
// DesktopReference supports offline mono Comb with Double storage/process only;
// Float32 accessors are empty for that explicit policy.
class PreparedIntervalHistogram {
public:
    static bool storage_bytes(std::size_t frames,IntervalHistogramConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,IntervalHistogramConfig={},std::span<const float> alpha={});
    void reset() noexcept;
    bool process_offline(std::span<const float> activations) noexcept;
    bool process_offline(std::span<const double>) noexcept;
    std::span<const double> reference_bins() const noexcept {return ready_?std::span<const double>(reference_bins_):std::span<const double>{};}
    bool process_online(std::span<const float> activations,bool reset_state=true) noexcept;
    std::span<const float> bins() const noexcept {return ready_?std::span<const float>(bins_):std::span<const float>{};}
    std::span<const float> channel_bins(std::size_t channel) const noexcept;
    std::size_t channels() const noexcept {return config_.channels;}
    std::span<const std::size_t> intervals() const noexcept {return intervals_;}
private:
    bool configure(std::size_t,IntervalHistogramConfig,std::span<const float> alpha);
    bool prepare_comb(std::span<const float> alpha);
    void allocate();
    bool validate(std::span<const float>) const noexcept;
    bool offline_acf(std::span<const float>) noexcept;
    bool response(std::span<const float>) noexcept;
    template<typename Scalar> bool retain_maxima(std::span<Scalar>) noexcept;
    template<typename Scalar> bool offline_comb(std::span<const Scalar>,std::span<Scalar>,std::span<Scalar>) noexcept;
    bool append(std::span<const float>) noexcept;
    bool summarize() noexcept;
    IntervalHistogramConfig config_;
    PreparedCombFilterBank comb_;
    std::size_t maximum_frames_=0,activation_cursor_=0,histogram_cursor_=0;
    bool ready_=false,streaming_=false;
    std::vector<std::size_t> intervals_;
    std::vector<double> reference_bins_,reference_response_;
    std::vector<float> bins_,response_,activations_,history_,absolute_,acf_;
};
}
