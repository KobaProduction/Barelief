// Adapted from madmom features/beats.py BeatTrackingProcessor.process at
// revision 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/local_interval_beat_search.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>
namespace dsp_kernels {
bool PreparedLocalIntervalBeatSearch::prepare(std::size_t frames,std::size_t ahead,std::size_t low,std::size_t high,float side) {
    std::size_t bound=0;if(!storage_bytes(frames,ahead,low,high,side,bound))return false;
    if(!frames || !ahead || !low || low>high || frames>std::size_t(PTRDIFF_MAX)/8 || ahead>std::size_t(PTRDIFF_MAX)/8 || high>2*ahead)return false;
    PreparedLocalIntervalBeatSearch next;
    TemporalWindowConfig config;config.length=2*ahead;config.hop=1;config.center=true;config.function=WindowFunction::Rectangular;
    if(!next.window_.prepare(config) || !next.search_.prepare(2*ahead,high,side))return false;
    next.maximum_frames_=frames;next.look_ahead_=ahead;next.minimum_interval_=low;next.maximum_interval_=high;
    next.context_.resize(2*ahead);next.positions_.resize(frames+1);next.intervals_.resize(frames+1);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedLocalIntervalBeatSearch::nearest(std::size_t prediction,std::size_t& result) const noexcept {
    const auto threshold=count_?positions_[count_-1]+minimum_interval_:0;
    const auto offset=std::ptrdiff_t(prediction)-std::ptrdiff_t(look_ahead_);
    std::size_t distance=SIZE_MAX;bool found=false;
    for(auto local:search_.positions()){
        const auto position=std::ptrdiff_t(local)+offset;
        if(position<0 || std::size_t(position)<threshold)continue;
        const auto delta=std::size_t(std::abs(position-std::ptrdiff_t(prediction)));
        if(delta<distance){distance=delta;result=std::size_t(position);found=true;}
    }
    return found;
}
bool PreparedLocalIntervalBeatSearch::next(std::span<const float> input,std::size_t prediction,IntervalEstimationPort estimator,std::size_t& next_prediction) noexcept {
    if(!window_.read(input,prediction,context_))return false;
    std::size_t interval=0;
    if(!estimator.estimate(estimator.context,context_,interval) || interval<minimum_interval_ || interval>maximum_interval_)return false;
    if(!search_.process(context_,interval))return false;
    std::size_t beat=0;if(!nearest(prediction,beat))return false;
    if(count_>=positions_.size())return false;
    positions_[count_]=beat;intervals_[count_]=interval;++count_;next_prediction=beat+interval;return true;
}
bool PreparedLocalIntervalBeatSearch::process(std::span<const float> input,IntervalEstimationPort estimator) noexcept {
    count_=0;
    if(!maximum_frames_ || !estimator.estimate || input.size()>maximum_frames_)return false;
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    std::size_t prediction=0;
    while(prediction<input.size())if(!next(input,prediction,estimator,prediction)){count_=0;return false;}
    return true;
}
}


namespace dsp_kernels {
bool PreparedLocalIntervalBeatSearch::storage_bytes(std::size_t frames,std::size_t ahead,std::size_t low,std::size_t high,float side,std::size_t& out) noexcept {
    if(!frames || !ahead || !low || low>high || frames>std::size_t(PTRDIFF_MAX)/8 || ahead>std::size_t(PTRDIFF_MAX)/8 || high>2*ahead)return false;
    TemporalWindowConfig c;c.length=2*ahead;c.hop=1;c.center=true;c.function=WindowFunction::Rectangular;
    std::size_t bytes=0,nested=0;
    if(!PreparedTemporalWindow::storage_bytes(c,{},bytes) || !PreparedIntervalBeatSearch::storage_bytes(2*ahead,high,side,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,2*ahead,sizeof(float)) ||
       !prepared_storage::add_array(bytes,frames+1,2*sizeof(std::size_t)))return false;
    out=bytes;return true;
}
}
