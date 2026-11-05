// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmRubato at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/tempo_change_sections.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedTempoChangeSections::required_workspace_bytes(std::size_t capacity,TempoChangeSectionConfig config,std::size_t& bytes) noexcept {
    if(!capacity || capacity>SIZE_MAX/(2*sizeof(float)) || !std::isfinite(config.tolerance) || config.tolerance<0 || config.tolerance>1)return false;
    if(!std::isfinite(config.long_region_seconds) || config.long_region_seconds<0 ||
       !std::isfinite(config.short_region_seconds) || config.short_region_seconds<0)return false;
    bytes=2*capacity*sizeof(float);return true;
}
bool PreparedTempoChangeSections::prepare(std::size_t capacity,TempoChangeSectionConfig config) {
    std::size_t bytes=0;if(!required_workspace_bytes(capacity,config,bytes))return false;
    PreparedTempoChangeSections next;next.config_=config;next.starts_.resize(capacity);next.stops_.resize(capacity);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedTempoChangeSections::validate(std::span<const float> beats) const noexcept {
    if(starts_.empty() || beats.size()>starts_.size())return false;
    if(!std::all_of(beats.begin(),beats.end(),[](float value){return std::isfinite(value);}))return false;
    if(beats.size()<=6)return true;
    for(std::size_t i=1;i<beats.size();++i)
        if(beats[i]<=beats[i-1] || !std::isfinite(60.f/(beats[i]-beats[i-1])))return false;
    return true;
}
bool PreparedTempoChangeSections::step(const std::array<float,5>& tempo) const noexcept {
    const float a=tempo[0],b=tempo[1],d=tempo[3],e=tempo[4],t=config_.tolerance;
    return std::abs(1.f-a/d)>=t && std::abs(1.f-b/e)>=t && std::abs(1.f-b/d)>=t &&
        std::abs(1.f-a/e)>=t && std::abs(1.f-a/b)<=t && std::abs(1.f-d/e)<=t;
}
void PreparedTempoChangeSections::open(float time) noexcept {
    if(stop_count_ && time-stops_[stop_count_-1]<config_.short_region_seconds)--stop_count_;
    else starts_[start_count_++]=time;
    open_=true;
}
void PreparedTempoChangeSections::close(float time) noexcept {
    if(start_count_ && time-starts_[start_count_-1]>config_.long_region_seconds)starts_[start_count_-1]=time;
    else if(stop_count_ && time-stops_[stop_count_-1]<config_.short_region_seconds)--stop_count_;
    else {stops_[stop_count_++]=time;open_=false;}
}
void PreparedTempoChangeSections::finish(float time) noexcept {
    if(!open_)return;
    if(start_count_ && time-starts_[start_count_-1]>config_.long_region_seconds)--start_count_;
    else stops_[stop_count_++]=time;
    open_=false;
}
void PreparedTempoChangeSections::advance(std::span<const float> beats) noexcept {
    std::array<float,5> tempo{};
    for(std::size_t i=0;i<tempo.size();++i)tempo[i]=60.f/(beats[5-i]-beats[4-i]);
    for(std::size_t i=6;i<beats.size();++i){
        if(step(tempo)){
            if(open_)close(beats[i-2]);
            else open(beats[i-2]);
        }
        for(std::size_t j=tempo.size()-1;j>0;--j)tempo[j]=tempo[j-1];
        tempo[0]=60.f/(beats[i]-beats[i-1]);
    }
    finish(beats.back());
}
bool PreparedTempoChangeSections::process(std::span<const float> beats) noexcept {
    if(!validate(beats))return false;
    start_count_=0;stop_count_=0;open_=false;
    if(beats.size()>6)advance(beats);
    return true;
}
}
