#pragma once
#include <dsp_kernels/interval_beat_search.h>
#include <dsp_kernels/temporal_window.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
struct IntervalEstimationPort {
    void* context=nullptr;
    bool (*estimate)(void*,std::span<const float>,std::size_t&) noexcept=nullptr;
};
// Variable-tempo source BeatTrackingProcessor: centered context -> explicit
// estimator -> interval beat search -> nearest admissible beat -> prediction.
// Input activations are already smoothed; estimator owns its histogram/smoothing.
// Callback borrows the context synchronously only. No graph or hidden Fs lookup.
// As in the source, a final prediction can lie beyond the last input sample.
class PreparedLocalIntervalBeatSearch {
public:
    static bool storage_bytes(std::size_t frames,std::size_t ahead,std::size_t low,std::size_t high,float side,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return search_.workspace_bytes()+window_.workspace_bytes()+context_.capacity()*sizeof(float)+(positions_.capacity()+intervals_.capacity())*sizeof(std::size_t);}
    bool prepare(std::size_t maximum_frames,std::size_t look_ahead_frames,
                 std::size_t minimum_interval,std::size_t maximum_interval,float look_aside=.2f);
    bool process(std::span<const float> activations,IntervalEstimationPort) noexcept;
    // One explicitly supplied interval for each possible prediction index.
    bool process(std::span<const float> activations,std::span<const std::size_t> intervals_by_prediction) noexcept;
    std::span<const std::size_t> positions() const noexcept {return std::span(positions_).first(count_);}
    std::span<const std::size_t> intervals() const noexcept {return std::span(intervals_).first(count_);}
private:
    bool configure(std::size_t frames,std::size_t ahead,std::size_t low,std::size_t high,float side);
    bool next(std::span<const float>,std::size_t,IntervalEstimationPort,std::size_t&) noexcept;
    bool nearest(std::size_t,std::size_t&) const noexcept;
    bool advance(std::size_t prediction,std::size_t interval,std::size_t& next_prediction) noexcept;
    PreparedIntervalBeatSearch search_;
    PreparedTemporalWindow window_;
    std::size_t maximum_frames_=0,look_ahead_=0,minimum_interval_=0,maximum_interval_=0,count_=0;
    std::vector<float> context_;
    std::vector<std::size_t> positions_,intervals_;
};
}
