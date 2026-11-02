// madmom features/beats.py CRFBeatDetectionProcessor composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/crf_beat_sequence.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/frame_timing.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedCrfBeatSequence::prepare(std::size_t maximum,CrfBeatSequenceConfig config,std::span<const float> factors) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,factors,bound))return false;
    PreparedCrfBeatSequence next;
    if(!next.configure(maximum,config,factors))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedCrfBeatSequence::configure(std::size_t frames,CrfBeatSequenceConfig c,std::span<const float> factors) {
    if(!frames || !c.num_intervals || (c.use_factors && factors.empty()) ||
        !std::all_of(factors.begin(),factors.end(),[](float x){return std::isfinite(x);}))return false;
    IntervalHistogramConfig bounds;const auto& h=c.tempo.histogram;
    if(!interval_histogram_parameters(h.frame_rate,h.minimum_bpm,h.maximum_bpm,h.history_seconds,bounds))return false;
    std::size_t smoothing=0;
    if(!truncated_frame_count(c.tempo.activation_smoothing_seconds,h.frame_rate,smoothing))return false;
    config_=c;maximum_frames_=frames;minimum_=bounds.minimum_interval;maximum_=bounds.maximum_interval;
    factors_.assign(factors.begin(),factors.end());activation_kernel_.resize(smoothing);
    if(!symmetric_hamming_window(activation_kernel_))return false;
    return prepare_numerics(frames,std::max(frames,smoothing));
}
bool PreparedCrfBeatSequence::prepare_numerics(std::size_t raw,std::size_t effective) {
    const auto trials=config_.use_factors?factors_.size():config_.num_intervals;
    if(!tempo_.prepare(raw,config_.tempo) || !search_.prepare(effective,minimum_,maximum_,trials,config_.interval_sigma))return false;
    intervals_.resize(trials);activations_.resize(effective);events_.resize(effective/minimum_);return true;
}
bool PreparedCrfBeatSequence::select_intervals() noexcept {
    trials_=0;const auto candidates=tempo_.candidates();const float numerator=60.f*config_.tempo.histogram.frame_rate;
    if(candidates.empty() || !std::isfinite(candidates[0].tempo) || candidates[0].tempo<=0)return false;
    if(config_.use_factors)return factor_beat_intervals(numerator/candidates[0].tempo,factors_,minimum_,maximum_,intervals_,trials_) && trials_>0;
    const auto count=std::min(candidates.size(),config_.num_intervals);
    for(std::size_t i=0;i<count;++i){
        const float interval=numerator/candidates[i].tempo;
        if(!std::isfinite(interval) || interval<1 || interval>=float(SIZE_MAX))return false;
        intervals_[trials_++]=std::size_t(interval);
    }
    return trials_>0;
}
bool PreparedCrfBeatSequence::publish() noexcept {
    const auto positions=search_.positions();
    if(positions.size()>events_.size())return false;
    for(std::size_t i=0;i<positions.size();++i){
        const float time=float(positions[i])/config_.tempo.histogram.frame_rate;
        if(!std::isfinite(time))return false;
        events_[i]=time;
    }
    count_=positions.size();ready_=true;return true;
}
bool PreparedCrfBeatSequence::process(std::span<const float> input) noexcept {
    ready_=false;tempo_ready_=false;search_attempted_=false;count_=0;trials_=0;
    if(!maximum_frames_ || input.size()>maximum_frames_ ||
        !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    if(!tempo_.process(input))return false;
    tempo_ready_=true;if(!select_intervals())return false;
    auto smoothed=std::span(activations_).first(same_convolution_size(input.size(),activation_kernel_.size()));
    if(!convolve_same(input,activation_kernel_,smoothed))return false;
    search_attempted_=true;
    if(!search_.process(smoothed,std::span(intervals_).first(trials_)))return false;
    return publish();
}
}


namespace dsp_kernels {
bool PreparedCrfBeatSequence::storage_bytes(std::size_t frames,CrfBeatSequenceConfig c,std::span<const float> factors,std::size_t& out) noexcept {
    if(!frames || !c.num_intervals || (c.use_factors && factors.empty()) || !std::all_of(factors.begin(),factors.end(),[](float v){return std::isfinite(v);}))return false;
    IntervalHistogramConfig limits;const auto& h=c.tempo.histogram;std::size_t smoothing=0;
    if(!interval_histogram_parameters(h.frame_rate,h.minimum_bpm,h.maximum_bpm,h.history_seconds,limits) ||
       !truncated_frame_count(c.tempo.activation_smoothing_seconds,h.frame_rate,smoothing))return false;
    const auto effective=std::max(frames,smoothing),trials=c.use_factors?factors.size():c.num_intervals;
    std::size_t bytes=0,nested=0;
    if(!PreparedTempoAnalysisSequence::storage_bytes(frames,c.tempo,{},{},{},bytes) ||
       !PreparedIntervalSequenceSearch::storage_bytes(effective,limits.minimum_interval,limits.maximum_interval,trials,c.interval_sigma,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,factors.size(),sizeof(float)) ||
       !prepared_storage::add_array(bytes,smoothing,sizeof(float)) || !prepared_storage::add_array(bytes,trials,sizeof(std::size_t)) ||
       !prepared_storage::add_array(bytes,effective,sizeof(float)) || !prepared_storage::add_array(bytes,effective/limits.minimum_interval,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedCrfBeatSequence::workspace_bytes() const noexcept {
    return tempo_.workspace_bytes()+search_.workspace_bytes()+(factors_.capacity()+activation_kernel_.capacity()+activations_.capacity()+events_.capacity())*sizeof(float)+
        intervals_.capacity()*sizeof(std::size_t);
}
}
