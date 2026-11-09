// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia HarmonicBpm/bpmutil at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/harmonic_tempo.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool positive_bpm(float value) noexcept {
    return std::isfinite(value) && value>=1;
}
bool tolerance_valid(float value) noexcept {
    return std::isfinite(value) && value>=0;
}
float integer_common_tempo(std::int32_t a,std::int32_t b,float tolerance,float error) noexcept {
    while(std::abs(error)>tolerance && b){
        HarmonicTempoDistance distance;
        if(!harmonic_tempo_distance(float(a),float(b),distance))return 0;
        error=distance.error_percent;
        const auto remainder=a%b;a=b;b=remainder;
    }
    return float(a);
}
}
bool harmonic_tempo_distance(float reference,float candidate,HarmonicTempoDistance& output) noexcept {
    if(!std::isfinite(reference) || !std::isfinite(candidate) || reference<=0 || candidate<=0)return false;
    HarmonicTempoDistance next;
    next.ratio=reference/candidate;
    if(next.ratio<1){
        next.ratio=std::round(1.f/next.ratio);
        next.error_percent=(reference*next.ratio-candidate)/std::min(candidate,reference*next.ratio)*100.f;
    }else{
        next.ratio=std::round(next.ratio);
        next.error_percent=(reference-candidate*next.ratio)/std::min(reference,candidate*next.ratio)*100.f;
    }
    if(!std::isfinite(next.ratio) || !std::isfinite(next.error_percent))return false;
    output=next;return true;
}
bool approximately_equal_tempo(float left,float right,float tolerance) noexcept {
    HarmonicTempoDistance distance;
    if(!tolerance_valid(tolerance) || !harmonic_tempo_distance(left,right,distance))return false;
    return std::abs(distance.error_percent)<=std::max(tolerance,std::numeric_limits<float>::epsilon()) && distance.ratio==1;
}
bool harmonically_related_tempo(float left,float right,float tolerance) noexcept {
    HarmonicTempoDistance distance;
    return tolerance_valid(tolerance) && harmonic_tempo_distance(left,right,distance) &&
        std::abs(distance.error_percent)<=std::max(tolerance,std::numeric_limits<float>::epsilon());
}
namespace {
bool bounded_common_tempo(float left,float right,float tolerance,float error,float& result) noexcept {
    const float maximum=std::nextafter(float(std::numeric_limits<std::int32_t>::max()),0.f);
    if(left>maximum || right>maximum)return false;
    result=integer_common_tempo(std::int32_t(left+.5f),std::int32_t(right+.5f),tolerance,error);
    return true;
}
}
bool greatest_common_tempo(float left,float right,float tolerance,float& result) noexcept {
    if(!tolerance_valid(tolerance))return false;
    if(left<right)std::swap(left,right);
    HarmonicTempoDistance distance;
    if(!harmonic_tempo_distance(left,right,distance))return false;
    tolerance=std::max(tolerance,std::numeric_limits<float>::epsilon());
    if(std::abs(distance.error_percent)<=tolerance){result=right;return true;}
    return bounded_common_tempo(left,right,tolerance,distance.error_percent,result);
}
bool PreparedHarmonicTempoCandidates::required_workspace_bytes(std::size_t capacity,HarmonicTempoConfig config,std::size_t& bytes) noexcept {
    if(!capacity || capacity>SIZE_MAX/(2*sizeof(float)) || !positive_bpm(config.reference_bpm) || !std::isfinite(config.minimum_common_bpm) ||
       config.minimum_common_bpm<1 || !tolerance_valid(config.tolerance_percent))return false;
    bytes=2*capacity*sizeof(float);return true;
}
bool PreparedHarmonicTempoCandidates::prepare(std::size_t capacity,HarmonicTempoConfig config) {
    std::size_t bytes=0;if(!required_workspace_bytes(capacity,config,bytes))return false;
    PreparedHarmonicTempoCandidates next;next.config_=config;next.candidates_.resize(capacity);next.result_.resize(capacity);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedHarmonicTempoCandidates::validate(std::span<const float> candidates) const noexcept {
    return !candidates_.empty() && candidates.size()<=candidates_.size() &&
        std::all_of(candidates.begin(),candidates.end(),positive_bpm);
}
bool PreparedHarmonicTempoCandidates::retain(std::span<const float> candidates,std::size_t& count) noexcept {
    count=0;
    for(const float candidate:candidates){
        float common=0;
        if(!greatest_common_tempo(config_.reference_bpm,candidate,config_.tolerance_percent,common))return false;
        if(common>=config_.minimum_common_bpm)candidates_[count++]=candidate;
    }
    std::sort(candidates_.begin(),candidates_.begin()+std::ptrdiff_t(count));return true;
}
float PreparedHarmonicTempoCandidates::error(float value) const noexcept {
    HarmonicTempoDistance distance;
    if(!harmonic_tempo_distance(config_.reference_bpm,value,distance))return std::numeric_limits<float>::infinity();
    return std::abs(distance.error_percent);
}
float PreparedHarmonicTempoCandidates::cluster(std::size_t& index,std::size_t count) const noexcept {
    const float first=candidates_[index];
    float best=first,minimum=std::numeric_limits<float>::infinity();
    while(index<count && approximately_equal_tempo(first,candidates_[index],config_.tolerance_percent)){
        const float current=error(candidates_[index]);
        if(current<minimum){minimum=current;best=candidates_[index];}
        ++index;
    }
    return best;
}
void PreparedHarmonicTempoCandidates::append(float best,float previous) noexcept {
    if(!approximately_equal_tempo(previous,best,config_.tolerance_percent))result_[count_++]=best;
    else if(error(result_[count_-1])>error(best))result_[count_-1]=best;
}
bool PreparedHarmonicTempoCandidates::process(std::span<const float> candidates) noexcept {
    if(!validate(candidates))return false;
    std::size_t retained=0;
    if(!retain(candidates,retained))return false;
    count_=0;float previous=-1;
    for(std::size_t index=0;index<retained;){
        const float best=cluster(index,retained);append(best,previous);previous=best;
    }
    return true;
}
}
