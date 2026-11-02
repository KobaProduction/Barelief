// madmom features/beats.py BeatDetectionProcessor / BeatTrackingProcessor,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/beat_tracking_sequence.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/frame_timing.h>
#include <dsp_kernels/histogram_tempo_peaks.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedBeatTrackingSequence::prepare(std::size_t maximum,BeatTrackingSequenceConfig config) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,bound))return false;
    PreparedBeatTrackingSequence next;
    if(!next.configure(maximum,config))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatTrackingSequence::configure(std::size_t maximum,BeatTrackingSequenceConfig c) {
    std::size_t smoothing=0,ahead=0;IntervalHistogramConfig limits;
    if(!maximum || !truncated_frame_count(c.activation_smoothing_seconds,c.histogram.frame_rate,smoothing) ||
        !interval_histogram_parameters(c.histogram.frame_rate,c.histogram.minimum_bpm,c.histogram.maximum_bpm,c.histogram.history_seconds,limits))return false;
    if(c.variable_tempo && !truncated_frame_count(c.look_ahead_seconds,c.histogram.frame_rate,ahead))return false;
    config_=c;maximum_frames_=maximum;activation_kernel_.resize(smoothing);histogram_kernel_.resize(c.histogram_smoothing);
    if(!symmetric_hamming_window(activation_kernel_) || !symmetric_hamming_window(histogram_kernel_))return false;
    const auto effective=std::max(maximum,smoothing);
    if(!prepare_search(effective,ahead,limits))return false;
    activations_.resize(effective);events_.resize(effective+1);return true;
}
bool PreparedBeatTrackingSequence::prepare_search(std::size_t effective,std::size_t ahead,const IntervalHistogramConfig& limits) {
    if(ahead>std::size_t(PTRDIFF_MAX)/8 || effective>std::size_t(PTRDIFF_MAX)/8)return false;
    const auto frames=config_.variable_tempo?std::max(effective,2*ahead):effective;
    if(!histogram_.prepare(frames,config_.histogram))return false;
    if(histogram_.offline_coordinates().size()!=histogram_.offline_bin_count() ||
        histogram_kernel_.size()>histogram_.offline_bin_count())return false;
    histogram_scratch_.resize(histogram_.offline_bin_count());
    if(config_.variable_tempo)return local_.prepare(effective,ahead,limits.minimum_interval,limits.maximum_interval,config_.look_aside);
    return global_.prepare(effective,limits.maximum_interval,config_.look_aside);
}
bool PreparedBeatTrackingSequence::estimate(std::span<const float> input,std::size_t& interval) noexcept {
    if(!histogram_.process_offline(input))return false;
    float value=0;
    if(!dominant_histogram_coordinate(histogram_.bins(),histogram_.coordinates(),histogram_kernel_,histogram_scratch_,value))return false;
    if(value<1 || value>=float(SIZE_MAX))return false;
    interval=std::size_t(value);return true;
}
bool PreparedBeatTrackingSequence::estimate_callback(void* context,std::span<const float> input,std::size_t& interval) noexcept {
    return static_cast<PreparedBeatTrackingSequence*>(context)->estimate(input,interval);
}
bool PreparedBeatTrackingSequence::publish(std::span<const std::size_t> positions) noexcept {
    if(positions.size()>events_.size())return false;
    for(std::size_t i=0;i<positions.size();++i){
        const float time=float(positions[i])/config_.histogram.frame_rate;
        if(!std::isfinite(time))return false;
        events_[i]=time;
    }
    count_=positions.size();ready_=true;return true;
}
std::span<const std::size_t> PreparedBeatTrackingSequence::positions() const noexcept {
    if(!ready_)return {};
    return config_.variable_tempo?local_.positions():global_.positions();
}
bool PreparedBeatTrackingSequence::process(std::span<const float> input,IntervalEstimationPort estimator) noexcept {
    count_=0;ready_=false;
    if(!maximum_frames_ || input.size()>maximum_frames_ ||
        !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    if(input.empty()){ready_=true;return true;}
    auto smooth=std::span(activations_).first(same_convolution_size(input.size(),activation_kernel_.size()));
    if(!convolve_same(input,activation_kernel_,smooth))return false;
    return search(smooth,estimator);
}
bool PreparedBeatTrackingSequence::search(std::span<const float> smooth,IntervalEstimationPort estimator) noexcept {
    if(!estimator.estimate)estimator={this,estimate_callback};
    if(config_.variable_tempo){
        if(!local_.process(smooth,estimator))return false;
        return publish(local_.positions());
    }
    std::size_t interval=0;
    if(!estimator.estimate(estimator.context,smooth,interval) || !global_.process(smooth,interval))return false;
    return publish(global_.positions());
}
}


namespace dsp_kernels {
bool PreparedBeatTrackingSequence::storage_bytes(std::size_t maximum,BeatTrackingSequenceConfig c,std::size_t& out) noexcept {
    std::size_t smoothing=0,ahead=0,bytes=0,nested=0;IntervalHistogramConfig limits;
    if(!maximum || !truncated_frame_count(c.activation_smoothing_seconds,c.histogram.frame_rate,smoothing) ||
       !interval_histogram_parameters(c.histogram.frame_rate,c.histogram.minimum_bpm,c.histogram.maximum_bpm,c.histogram.history_seconds,limits) ||
       (c.variable_tempo && !truncated_frame_count(c.look_ahead_seconds,c.histogram.frame_rate,ahead)))return false;
    const auto effective=std::max(maximum,smoothing),bins=limits.maximum_interval-limits.minimum_interval+1;
    if(ahead>std::size_t(PTRDIFF_MAX)/8 || effective>std::size_t(PTRDIFF_MAX)/8 || c.histogram_smoothing>bins ||
       !PreparedTempoHistogramSequence::storage_bytes(c.variable_tempo?std::max(effective,2*ahead):effective,c.histogram,{},bytes))return false;
    const bool search=c.variable_tempo?PreparedLocalIntervalBeatSearch::storage_bytes(effective,ahead,limits.minimum_interval,limits.maximum_interval,c.look_aside,nested):
        PreparedIntervalBeatSearch::storage_bytes(effective,limits.maximum_interval,c.look_aside,nested);
    if(!search || !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,smoothing,sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.histogram_smoothing,sizeof(float)) || !prepared_storage::add_array(bytes,effective,sizeof(float)) ||
       !prepared_storage::add_array(bytes,effective+1,sizeof(float)) || !prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedBeatTrackingSequence::workspace_bytes() const noexcept {
    return histogram_.workspace_bytes()+global_.workspace_bytes()+local_.workspace_bytes()+
        (activation_kernel_.capacity()+histogram_kernel_.capacity()+activations_.capacity()+histogram_scratch_.capacity()+events_.capacity())*sizeof(float);
}
}
