// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia RhythmExtractor postprocessing at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/tick_sequence_refinement.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedTickSequenceRefinement::prepare(std::size_t capacity,TickSequenceRefinementConfig config) {
    std::size_t admitted=0;if(!storage_bytes(capacity,config,admitted))return false;
    if(!capacity || !std::isfinite(config.frame_rate) || config.frame_rate<=0 ||
       !std::isfinite(config.minimum_interval_seconds) || config.minimum_interval_seconds<0 ||
       !std::isfinite(config.last_beat_interval_seconds) || config.last_beat_interval_seconds<0)return false;
    PreparedTickSequenceRefinement next;next.config_=config;
    next.ticks_.resize(capacity);next.intervals_.resize(capacity-1);if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedTickSequenceRefinement::extend(float file_length) noexcept {
    if(file_length<=ticks_[count_-1])return true;
    const float period=ticks_[count_-1]-ticks_[count_-2],limit=file_length-config_.last_beat_interval_seconds;
    while(ticks_[count_-1]+period<limit){
        const float next=ticks_[count_-1]+period;
        if(!std::isfinite(next) || next<=ticks_[count_-1]){status_=TickSequenceStatus::NonAdvancingExtension;return false;}
        if(count_==ticks_.size()){status_=TickSequenceStatus::CapacityExceeded;return false;}
        ticks_[count_++]=next;
    }
    return std::isfinite(period);
}
void PreparedTickSequenceRefinement::clip(float begin,float end) noexcept {
    std::size_t accepted=0;
    for(std::size_t i=0;i<count_;++i)if(ticks_[i]>=begin && ticks_[i]<=end)ticks_[accepted++]=ticks_[i];
    count_=accepted;
}
void PreparedTickSequenceRefinement::separate() noexcept {
    if(!count_)return;
    std::size_t accepted=1;
    for(std::size_t i=1;i<count_;++i)
        if(ticks_[i]-ticks_[accepted-1]>=config_.minimum_interval_seconds)ticks_[accepted++]=ticks_[i];
    count_=accepted;
}
bool PreparedTickSequenceRefinement::backward_offbeat(std::size_t i) const noexcept {
    return std::abs((ticks_[i]-ticks_[i-2])-1.5f*(ticks_[i]-ticks_[i-1]))<.1f &&
        std::abs(ticks_[i]-ticks_[i-1]-ticks_[i-2]+ticks_[i-3])<.1f;
}
void PreparedTickSequenceRefinement::remove_offbeats() noexcept {
    for(std::size_t i=3;i<count_;++i)if(backward_offbeat(i)){
        std::move(ticks_.begin()+std::ptrdiff_t(i-1),ticks_.begin()+std::ptrdiff_t(count_),ticks_.begin()+std::ptrdiff_t(i-2));
        --count_;--i;
    }
}
bool PreparedTickSequenceRefinement::finalize() noexcept {
    for(std::size_t i=1;i<count_;++i){
        intervals_[i-1]=ticks_[i]-ticks_[i-1];
        if(!std::isfinite(intervals_[i-1]))return false;
    }
    status_=TickSequenceStatus::Ready;return true;
}
bool PreparedTickSequenceRefinement::process(std::span<const float> candidates,std::uint64_t frames,float start,float stop) noexcept {
    count_=0;status_=TickSequenceStatus::InvalidInput;
    if(ticks_.empty() || !std::isfinite(start) || start<0 || !std::isfinite(stop) || stop<0 ||
       !std::all_of(candidates.begin(),candidates.end(),[](float x){return std::isfinite(x);}))return false;
    if(candidates.size()>ticks_.size()){status_=TickSequenceStatus::CapacityExceeded;return false;}
    const float length=float(frames)/config_.frame_rate,begin=start/config_.frame_rate;
    const float end=std::max(stop,float(frames)-5.f*config_.frame_rate)/config_.frame_rate-config_.last_beat_interval_seconds;
    if(!std::isfinite(length) || !std::isfinite(begin) || !std::isfinite(end))return false;
    std::copy(candidates.begin(),candidates.end(),ticks_.begin());count_=candidates.size();
    if(count_>1){
        if(!extend(length))return false;
        clip(begin,end);separate();remove_offbeats();
    }
    return finalize();
}
std::span<const float> PreparedTickSequenceRefinement::ticks() const noexcept {
    return status_==TickSequenceStatus::Ready?std::span<const float>(ticks_).first(count_):std::span<const float>{};
}
std::span<const float> PreparedTickSequenceRefinement::intervals() const noexcept {
    return status_==TickSequenceStatus::Ready?std::span<const float>(intervals_).first(count_>1?count_-1:0):std::span<const float>{};
}
}
