#pragma once
#include <dsp_kernels/tempo_histogram_sequence.h>
#include <dsp_kernels/histogram_tempo_peaks.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
struct TempoAnalysisSequenceConfig {
    TempoHistogramSequenceConfig histogram;
    float activation_smoothing_seconds=.14f;
    std::size_t histogram_smoothing=9;
    bool interpolate=false,smooth_state_model_activations=false;
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
// Source TempoEstimationProcessor composition: optional unnormalized symmetric
// Hamming activation smoothing (offline only), selected histogram, histogram
// smoothing, then shared detect_tempo. Explicit kernels override integer window
// recipes. DesktopReference retains Double smoothing, comb, and strict peak
// decisions for offline mono Comb without interpolation; published views stay
// Float32. Unsupported reference combinations reject atomically. A StateModel uses supplied probabilities and disables activation
// smoothing by default. No neural inference or weights.
// Empty online calls query the current histogram instead of the source's
// uninitialized local variable. The missing source interval_histogram attribute
// is explicitly bound to the selected histogram processor.
// Source-invalid axis mismatches (log DBN bincount or smoothing longer than its
// histogram axis) are rejected at preparation; raw histograms remain available
// through PreparedTempoHistogramSequence.
class PreparedTempoAnalysisSequence {
public:
    static bool storage_bytes(std::size_t,TempoAnalysisSequenceConfig,std::span<const float>,std::span<const float>,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,TempoAnalysisSequenceConfig={},
                 std::span<const float> activation_kernel={},std::span<const float> histogram_kernel={},
                 std::span<const float> comb_alpha={});
    void reset() noexcept;
    bool process(std::span<const float>,bool reset_state=true) noexcept;
    bool process_offline(std::span<const float>) noexcept;
    bool process_online(std::span<const float>,bool reset_state=true) noexcept;
    std::span<const HistogramTempo> candidates() const noexcept;
    std::span<const float> raw_histogram() const noexcept {return reference()?std::span<const float>(reference_raw_).first(ready_?reference_raw_.size():0):histogram_.bins();}
    std::span<const float> histogram_coordinates() const noexcept {return reference()?std::span<const float>(reference_coordinates_):histogram_.coordinates();}
    std::span<const float> smoothed_histogram() const noexcept {return std::span(smoothed_bins_).first(ready_?raw_histogram().size():0);}
    std::span<const HistogramTempoReference> reference_candidates() const noexcept {return ready_?offline_peaks_.reference_candidates():std::span<const HistogramTempoReference>{};}
private:
    bool reference() const noexcept {return config_.precision==NumericalPrecision::DesktopReference;}
    bool configure_reference(std::size_t,std::span<const float>,std::span<const float>,std::span<const float>);
    bool process_reference(std::span<const float>) noexcept;
    static bool reference_storage(std::size_t,TempoAnalysisSequenceConfig,std::size_t,std::size_t,std::span<const float>,std::size_t&) noexcept;
    bool configure(std::size_t,TempoAnalysisSequenceConfig,std::span<const float>,std::span<const float>,std::span<const float>);
    bool prepare_windows(std::span<const float>,std::span<const float>);
    bool prepare_histogram(std::size_t,std::span<const float> comb_alpha);
    bool extract(bool online) noexcept;
    bool valid_input(std::span<const float>) const noexcept;
    TempoAnalysisSequenceConfig config_;
    PreparedTempoHistogramSequence histogram_;
    PreparedIntervalHistogram reference_histogram_;
    std::vector<double> reference_activation_kernel_,reference_histogram_kernel_,reference_input_,reference_activations_,reference_smoothed_;
    std::vector<float> reference_coordinates_,reference_raw_;
    PreparedHistogramTempoPeaks offline_peaks_,online_peaks_;
    std::vector<float> activation_kernel_,histogram_kernel_,activations_,smoothed_bins_;
    std::size_t maximum_frames_=0;
    bool ready_=false,last_online_=false;
};
}
