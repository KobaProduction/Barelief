// Adapted from madmom features/beats.py detect_beats at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/interval_beat_search.h>
#include <dsp_kernels/dsp.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
void PreparedIntervalBeatSearch::allocate_results(std::size_t frames,std::size_t interval,float look_aside) {
    maximum_frames_=frames;maximum_interval_=interval;look_aside_=look_aside;
    scores_.resize(interval);positions_.resize(frames);
}
bool PreparedIntervalBeatSearch::prepare(std::size_t frames,std::size_t interval,float look_aside) {
    std::size_t bound=0;if(!storage_bytes(frames,interval,look_aside,bound))return false;
    const auto radius=std::max(std::size_t{1},std::size_t(float(interval)*look_aside));
    PreparedIntervalBeatSearch next;if(!next.prepare_windows(radius))return false;
    next.allocate_results(frames,interval,look_aside);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedIntervalBeatSearch::prepare_windows(std::size_t radius) {
    if(radius==SIZE_MAX || radius>SIZE_MAX/(radius+1))return false;
    window_.resize(radius*(radius+1));
    for(std::size_t i=1;i<=radius;++i)window(std::span(window_).subspan(i*(i-1),2*i),WindowFunction::Hamming,false);
    return true;
}
float PreparedIntervalBeatSearch::weighted(std::span<const float> input,std::ptrdiff_t frame,std::size_t bin) const noexcept {
    return frame>=0 && std::size_t(frame)<input.size()?input[std::size_t(frame)]*window_[radius_*(radius_-1)+bin]:0;
}
std::size_t PreparedIntervalBeatSearch::refine(std::span<const float> input,std::size_t position) const noexcept {
    const auto start=std::ptrdiff_t(position)-std::ptrdiff_t(radius_);
    std::size_t peak=0;float maximum=weighted(input,start,0);
    for(std::size_t i=1;i<2*radius_;++i){
        const float value=weighted(input,start+std::ptrdiff_t(i),i);
        if(value>maximum){maximum=value;peak=i;}
    }
    // The source deliberately ignores an argmax at index zero.
    return peak?std::size_t(start+std::ptrdiff_t(peak)):position;
}
bool PreparedIntervalBeatSearch::retain_position(std::size_t position,bool retain) noexcept {
    if(!retain)return true;
    if(count_>=positions_.size())return false;
    positions_[count_++]=position;
    return true;
}
bool PreparedIntervalBeatSearch::select_phase(std::span<const float> input,std::size_t interval) noexcept {
    const auto best=std::size_t(std::max_element(scores_.begin(),scores_.begin()+std::ptrdiff_t(interval))-scores_.begin());
    float score=0;
    if(!sequence(input,interval,best,true,score)){count_=0;return false;}
    result_interval_=interval;
    return true;
}
bool PreparedIntervalBeatSearch::score_position(std::span<const float> input,std::size_t& position,bool retain,float& score) noexcept {
    position=refine(input,position);
    if(position>=input.size())return false;
    score+=input[position];
    return std::isfinite(score) && retain_position(position,retain);
}
bool PreparedIntervalBeatSearch::sequence(std::span<const float> input,std::size_t interval,std::size_t start,bool retain,float& score) noexcept {
    score=0;auto position=start;
    while(position<input.size()){
        const auto prediction=position;
        if(!score_position(input,position,retain,score))return false;
        if(interval>=input.size()-position)break;
        position+=interval;if(position<=prediction)return false;
    }
    return true;
}
bool PreparedIntervalBeatSearch::score_phases(std::span<const float> input,std::size_t interval) noexcept {
    for(std::size_t phase=0;phase<interval;++phase)
        if(!sequence(input,interval,phase,false,scores_[phase]))return false;
    return true;
}
bool PreparedIntervalBeatSearch::process(std::span<const float> input,std::size_t interval) noexcept {
    count_=0;result_interval_=0;
    if(!maximum_frames_ || input.size()>maximum_frames_ || !interval || interval>maximum_interval_ || interval>input.size())return false;
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    radius_=std::max(std::size_t{1},std::size_t(float(interval)*look_aside_));
    if(radius_>interval)return false;
    if(!score_phases(input,interval))return false;
    return select_phase(input,interval);
}
}


namespace dsp_kernels {
bool PreparedIntervalBeatSearch::storage_bytes(std::size_t frames,std::size_t interval,float side,std::size_t& out) noexcept {
    if(!frames || !interval || interval>frames || frames>std::size_t(PTRDIFF_MAX)/4 || !std::isfinite(side) || side<0 || side>1)return false;
    const auto radius=std::max(std::size_t{1},std::size_t(float(interval)*side));std::size_t windows=0,bytes=0;
    if(radius==SIZE_MAX || !prepared_storage::mul_bytes(radius,radius+1,windows) || !prepared_storage::add_array(bytes,windows,sizeof(float)) ||
       !prepared_storage::add_array(bytes,interval,sizeof(float)) || !prepared_storage::add_array(bytes,frames,sizeof(std::size_t)))return false;
    out=bytes;return true;
}
}
