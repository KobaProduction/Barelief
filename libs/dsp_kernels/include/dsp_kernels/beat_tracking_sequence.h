#pragma once
#include <dsp_kernels/tempo_histogram_sequence.h>
#include <dsp_kernels/interval_beat_search.h>
#include <dsp_kernels/local_interval_beat_search.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
struct BeatTrackingSequenceConfig {
    TempoHistogramSequenceConfig histogram;
    float activation_smoothing_seconds=.14f,look_aside=.2f,look_ahead_seconds=10;
    std::size_t histogram_smoothing=9;
    bool variable_tempo=true;
};
// Source BeatTrackingProcessor / BeatDetectionProcessor (variable_tempo=false).
// Uses truncated activation smoothing, interval histogram + smoothed first
// argmax, then shared global/local beat search. No tempo-estimation smoothing
// is accidentally applied twice. Explicit estimator overrides the built-in
// histogram synchronously and is never retained. Missing legacy Python
// estimator attributes are bound to these explicit operations.
class PreparedBeatTrackingSequence {
public:
    static bool storage_bytes(std::size_t,BeatTrackingSequenceConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,BeatTrackingSequenceConfig={});
    bool process(std::span<const float>,IntervalEstimationPort override_estimator={}) noexcept;
    std::span<const float> events() const noexcept {return std::span(events_).first(count_);}
    std::span<const std::size_t> positions() const noexcept;
    std::span<const float> histogram() const noexcept {return histogram_.bins();}
private:
    bool configure(std::size_t,BeatTrackingSequenceConfig);
    bool prepare_search(std::size_t,std::size_t,const IntervalHistogramConfig&);
    bool estimate(std::span<const float>,std::size_t&) noexcept;
    static bool estimate_callback(void*,std::span<const float>,std::size_t&) noexcept;
    bool publish(std::span<const std::size_t>) noexcept;
    bool search(std::span<const float>,IntervalEstimationPort) noexcept;
    BeatTrackingSequenceConfig config_;
    PreparedTempoHistogramSequence histogram_;
    PreparedIntervalBeatSearch global_;
    PreparedLocalIntervalBeatSearch local_;
    std::vector<float> activation_kernel_,histogram_kernel_,activations_,histogram_scratch_,events_;
    std::size_t maximum_frames_=0,count_=0;
    bool ready_=false;
};
}
