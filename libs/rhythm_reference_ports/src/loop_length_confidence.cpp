// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia LoopBpmConfidence at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/loop_length_confidence.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace rhythm_reference_ports {
namespace {
float duration_score(std::int64_t duration,std::int64_t original,float beat) noexcept {
    auto distance=original;
    for(std::int64_t beats=1;beats<128;++beats){
        const auto expected=std::int64_t(std::round(beat*float(beats)));
        distance=std::min(distance,std::abs(duration-expected));
    }
    const float tolerance=beat*.5f;
    return float(distance)>tolerance?0.f:1.f-float(distance)/tolerance;
}
}
bool PreparedLoopLengthConfidence::prepare(float rate) noexcept {
    const float maximum=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(rate) || rate<1 || rate>maximum)return false;
    dsp_kernels::EnvelopeFollowerConfig config;config.sample_rate=std::trunc(rate);
    config.attack_seconds=.01f;config.release_seconds=.01f;
    if(!envelope_.prepare(config))return false;
    sample_rate_=rate;return true;
}
bool PreparedLoopLengthConfidence::maximum(std::span<const float> input,float& result) noexcept {
    envelope_.reset();result=0;
    for(const float sample:input){
        float value=0;
        if(!envelope_.process(sample,value))return false;
        result=std::max(result,value);
    }
    return true;
}
bool PreparedLoopLengthConfidence::edges(std::span<const float> input,float threshold,LoopLengthConfidenceResult& result) noexcept {
    envelope_.reset();bool first=true;
    for(std::size_t i=0;i<input.size();++i){
        float value=0;
        if(!envelope_.process(input[i],value))return false;
        if(value<threshold)continue;
        if(first){result.start_sample=i;first=false;}
        result.end_sample=i;
    }
    return true;
}
bool PreparedLoopLengthConfidence::score(std::size_t samples,LoopLengthConfidenceResult& result) const noexcept {
    const auto duration=std::int64_t(samples),start=std::int64_t(result.start_sample),end=std::int64_t(result.end_sample);
    result.durations={duration,duration-start,end,end-start};
    const float beat=60.f*sample_rate_/result.rounded_bpm;
    if(!std::isfinite(beat) || beat*.5f<=0)return false;
    for(std::size_t i=0;i<result.scores.size();++i)result.scores[i]=duration_score(result.durations[i],duration,beat);
    result.confidence=*std::max_element(result.scores.begin(),result.scores.end());
    return std::isfinite(result.confidence);
}
bool PreparedLoopLengthConfidence::process(std::span<const float> input,float bpm,LoopLengthConfidenceResult& output) noexcept {
    if(sample_rate_<=0 || input.size()>std::size_t(std::numeric_limits<int>::max()) || !std::isfinite(bpm))return false;
    LoopLengthConfidenceResult next;next.rounded_bpm=std::round(bpm);
    if(next.rounded_bpm<=0 || input.empty()){output=next;return true;}
    float peak=0;
    if(!maximum(input,peak) || !edges(input,peak*.05f,next) || !score(input.size(),next))return false;
    output=next;return true;
}
bool select_loop_tempo(float estimate,float confidence,float threshold,float& bpm) noexcept {
    if(!std::isfinite(estimate) || !std::isfinite(confidence) || confidence<0 || confidence>1 ||
       !std::isfinite(threshold) || threshold<0 || threshold>1)return false;
    bpm=confidence>=threshold?std::round(estimate):0.f;return true;
}
}
