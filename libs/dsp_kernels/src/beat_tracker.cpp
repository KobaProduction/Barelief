// Adapted from librosa 1.0.0, librosa/beat.py: __beat_tracker and helpers.
// https://github.com/librosa/librosa/blob/1.0.0/librosa/beat.py
// ISC License
// Copyright (c) 2013--2026, librosa development team.
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/beat_tracker.h>
#include <dsp_kernels/order_statistics.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp_kernels {
namespace {
using Sum=Float32Accumulator;
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
std::int32_t round_even(float value) noexcept {
    const float lower=std::floor(value),fraction=value-lower;
    const auto integer=std::int32_t(lower);
    return integer+std::int32_t(fraction>.5f || (fraction==.5f && integer%2));
}
bool valid(const BeatTrackerConfig& c) noexcept {
    return std::isfinite(c.frame_rate) && c.frame_rate>0 && std::isfinite(c.frame_rate*60.f) &&
        std::isfinite(c.tightness) && c.tightness>0 && c.maximum_period_frames>=2 && c.maximum_period_frames<=INT32_MAX/4;
}
bool period_frames(float bpm,const BeatTrackerConfig& config,std::int32_t& out) noexcept {
    if(!std::isfinite(bpm) || bpm<=0)return false;
    const float period=config.frame_rate*60.f/bpm;
    if(!std::isfinite(period) || period<.5f || period>float(config.maximum_period_frames))return false;
    out=round_even(period);return out>=2 && std::size_t(out)<=config.maximum_period_frames;
}
bool add_bytes(std::size_t count,std::size_t width,std::size_t& bytes) noexcept {
    if(count>(SIZE_MAX-bytes)/width)return false;
    bytes+=count*width;return true;
}
bool local_maximum(std::span<const float> values,std::size_t i) noexcept {
    return i>0 && values[i]>values[i-1] && (i+1==values.size() || values[i]>=values[i+1]);
}
float median(std::span<float> values) noexcept {
    if(values.empty())return 0;
    return median_in_place(values,EvenMedian::HalfSum);
}
}
bool PreparedBeatTracker::storage_bytes(std::size_t frames,const BeatTrackerConfig& c,std::size_t& out) noexcept {
    if(!valid(c) || !frames || frames>INT32_MAX/4)return false;
    std::size_t bytes=sizeof(PreparedBeatTracker);
    if(!add_bytes(frames,4*sizeof(float)+2*sizeof(std::int32_t)+sizeof(std::uint8_t),bytes) ||
       !add_bytes(2*c.maximum_period_frames+1,2*sizeof(float),bytes))return false;
    out=bytes;return true;
}
bool PreparedBeatTracker::prepare(std::size_t frames,const BeatTrackerConfig& c) {
    std::size_t bytes=0;if(!storage_bytes(frames,c,bytes))return false;
    PreparedBeatTracker next;next.maximum_frames_=frames;next.config_=c;
    next.allocate(frames);
    for(std::size_t i=1;i<next.log_intervals_.size();++i)next.log_intervals_[i]=std::log(float(i));
    if(next.workspace_bytes()>bytes-sizeof(PreparedBeatTracker))return false;
    *this=std::move(next);return true;
}
void PreparedBeatTracker::allocate(std::size_t frames) {
    normalized_.resize(frames);local_.resize(frames);cumulative_.resize(frames);scratch_.resize(frames);
    periods_.resize(frames);backlink_.resize(frames);mask_.resize(frames);
    window_.resize(2*config_.maximum_period_frames+1);log_intervals_.resize(2*config_.maximum_period_frames+1);
}
bool PreparedBeatTracker::input(std::span<const float> envelope,std::span<const float> bpm) noexcept {
    if(!maximum_frames_ || envelope.size()>maximum_frames_ || (bpm.size()!=1 && bpm.size()!=envelope.size()) || !finite(envelope))return false;
    for(std::size_t i=0;i<bpm.size();++i)if(!period_frames(bpm[i],config_,periods_[i]))return false;
    if(bpm.size()==1)std::fill_n(periods_.begin(),envelope.size(),periods_[0]);
    return true;
}
bool PreparedBeatTracker::normalize(std::span<const float> envelope) noexcept {
    float deviation=0;
    if(!sample_deviation_float32(envelope,deviation,Float32Summation::Compensated,true))return false;
    const float denominator=deviation+std::numeric_limits<float>::min();
    for(std::size_t i=0;i<envelope.size();++i)normalized_[i]=envelope[i]/denominator;
    return std::isfinite(denominator) && finite(std::span(normalized_).first(envelope.size()));
}
void PreparedBeatTracker::window(std::int32_t period) noexcept {
    for(std::int32_t k=-period;k<=period;++k){
        const float x=float(k)*32.f/float(period);window_[std::size_t(k+period)]=std::exp(-.5f*x*x);
    }
}
bool PreparedBeatTracker::local_scores(std::size_t frames) noexcept {
    std::int32_t previous=0;
    for(std::size_t i=0;i<frames;++i){
        const auto p=periods_[i],k_count=2*p+1;
        if(p!=previous){window(p);previous=p;}
        const auto begin=std::max(std::int32_t{0},std::int32_t(i)+p-std::int32_t(frames)+1),end=std::min(std::int32_t(i)+p,k_count);
        float value=0;
        // Preserve the original exclusive upper bound, including its boundary.
        for(auto k=begin;k<end;++k)value+=window_[std::size_t(k)]*normalized_[i+std::size_t(p)-std::size_t(k)];
        local_[i]=value;
    }
    return finite(std::span(local_).first(frames));
}
void PreparedBeatTracker::advance(std::size_t i,float threshold,bool& first) noexcept {
    const auto p=periods_[i],start=std::int32_t(i)-round_even(float(p)*.5f),end=std::max(std::int32_t{0},std::int32_t(i)-2*p);
    float best=-std::numeric_limits<float>::infinity();std::int32_t link=-1;
    for(auto loc=start;loc>=end;--loc){
        const float delta=log_intervals_[i-std::size_t(loc)]-log_intervals_[std::size_t(p)];
        const float score=cumulative_[std::size_t(loc)]-config_.tightness*delta*delta;
        if(score>best){best=score;link=loc;}
    }
    cumulative_[i]=local_[i]+(link>=0?best:0);
    if(first && local_[i]<threshold)backlink_[i]=-1;
    else {backlink_[i]=link;first=false;}
}
bool PreparedBeatTracker::dynamic_program(std::size_t frames) noexcept {
    const float threshold=.01f**std::max_element(local_.begin(),local_.begin()+std::ptrdiff_t(frames));bool first=true;
    for(std::size_t i=0;i<frames;++i)advance(i,threshold,first);
    return finite(std::span(cumulative_).first(frames));
}
std::size_t PreparedBeatTracker::tail(std::size_t frames) noexcept {
    const auto scores=std::span(cumulative_).first(frames);std::size_t count=0;
    for(std::size_t i=0;i<frames;++i)if(local_maximum(scores,i))scratch_[count++]=scores[i];
    const float threshold=.5f*median(std::span(scratch_).first(count));
    for(std::size_t i=frames;i-->0;)if(local_maximum(scores,i) && scores[i]>=threshold)return i;
    return frames-1;
}
float PreparedBeatTracker::trim_threshold(std::size_t frames) noexcept {
    if(!config_.trim)return 0;
    std::size_t count=0;for(std::size_t i=0;i<frames;++i)if(mask_[i])scratch_[count++]=local_[i];
    const auto length=std::min(frames,count+2);Sum squares;
    for(std::size_t i=0;i<length;++i){
        const float value=(i<count?scratch_[i]:0)+(i+1<count?.5f*scratch_[i+1]:0)+(i && i-1<count?.5f*scratch_[i-1]:0);
        squares.add(value*value);
    }
    return .5f*std::sqrt(squares.value/float(length));
}
bool PreparedBeatTracker::trim(std::size_t frames) noexcept {
    const float threshold=trim_threshold(frames);
    if(!std::isfinite(threshold))return false;
    for(std::size_t i=0;i<frames && local_[i]<=threshold;++i)mask_[i]=0;
    for(std::size_t i=frames;i && local_[i-1]<=threshold;--i)mask_[i-1]=0;
    return true;
}
void PreparedBeatTracker::clear(std::size_t frames) noexcept {
    std::fill_n(mask_.begin(),frames,0);
    std::fill_n(local_.begin(),frames,0);std::fill_n(cumulative_.begin(),frames,0);std::fill_n(backlink_.begin(),frames,-1);
}
bool PreparedBeatTracker::estimate(std::span<const float> envelope) noexcept {
    const auto frames=envelope.size();
    if(frames<2 || std::none_of(envelope.begin(),envelope.end(),[](float x){return x>0;}))return true;
    if(!normalize(envelope) || !local_scores(frames) || !dynamic_program(frames))return false;
    if(*std::max_element(local_.begin(),local_.begin()+std::ptrdiff_t(frames))<=0)return true;
    for(auto i=std::int32_t(tail(frames));i>=0;i=backlink_[std::size_t(i)])mask_[std::size_t(i)]=1;
    return trim(frames);
}
bool PreparedBeatTracker::process(std::span<const float> envelope,std::span<const float> bpm,std::span<std::uint8_t> beats) noexcept {
    result_frames_=0;if(beats.size()!=envelope.size() || !input(envelope,bpm))return false;
    const auto frames=envelope.size();clear(frames);
    if(!estimate(envelope))return false;
    std::copy_n(mask_.begin(),frames,beats.begin());result_frames_=frames;return true;
}
bool PreparedBeatTracker::silence(std::size_t frames,std::span<std::uint8_t> beats) noexcept {
    if(!maximum_frames_ || frames>maximum_frames_ || beats.size()!=frames)return false;
    clear(frames);std::fill(beats.begin(),beats.end(),0);result_frames_=frames;return true;
}
}


namespace dsp_kernels {
std::size_t PreparedBeatTracker::workspace_bytes() const noexcept {
    return (normalized_.capacity()+local_.capacity()+cumulative_.capacity()+scratch_.capacity()+window_.capacity()+log_intervals_.capacity())*sizeof(float)+
        (periods_.capacity()+backlink_.capacity())*sizeof(std::int32_t)+mask_.capacity();
}
}
