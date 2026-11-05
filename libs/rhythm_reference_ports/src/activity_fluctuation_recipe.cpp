// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia Danceability at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/activity_fluctuation_recipe.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedActivityFluctuation::build_scales(ActivityFluctuationConfig config) {
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(config.minimum_tau_ms) || !std::isfinite(config.maximum_tau_ms) ||
       !std::isfinite(config.tau_multiplier) || config.minimum_tau_ms<=0 ||
       config.maximum_tau_ms<config.minimum_tau_ms || config.maximum_tau_ms/10.f>bound ||
       config.tau_multiplier<=1)return false;
    std::vector<std::size_t> scales;
    for(float tau=config.minimum_tau_ms;tau<=config.maximum_tau_ms;){
        const float value=tau/10.f;
        if(value<2)return false;
        scales.push_back(std::size_t(value));
        const float next=tau*config.tau_multiplier;
        if(next==tau)return false;
        tau=next;
    }
    return scaling_.prepare(scales);
}
bool PreparedActivityFluctuation::configure(std::size_t maximum,ActivityFluctuationConfig config) {
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    const float frame=.01f*config.sample_rate;
    if(!maximum || maximum>std::size_t(std::numeric_limits<int>::max()) ||
       !std::isfinite(frame) || frame<2 || frame>bound || !build_scales(config))return false;
    maximum_pcm_=maximum;frame_size_=std::size_t(frame);activity_.resize(maximum/frame_size_);return true;
}
bool PreparedActivityFluctuation::prepare(std::size_t maximum,ActivityFluctuationConfig config) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,bound))return false;
    PreparedActivityFluctuation next;
    if(!next.configure(maximum,config) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedActivityFluctuation::integrate(std::span<const float> pcm) noexcept {
    activity_count_=pcm.size()/frame_size_;
    auto values=std::span(activity_).first(activity_count_);
    for(std::size_t frame=0;frame<values.size();++frame)
        if(!dsp_kernels::sample_deviation_float32(pcm.subspan(frame*frame_size_,frame_size_),values[frame]))return false;
    if(values.empty())return true;
    float mean=0;
    if(!dsp_kernels::arithmetic_mean_float32(values,mean))return false;
    for(auto& value:values)value-=mean;
    for(std::size_t i=1;i<values.size();++i)values[i]+=values[i-1];
    return true;
}
bool PreparedActivityFluctuation::process(std::span<const float> pcm) noexcept {
    activity_count_=0;
    if(!frame_size_ || pcm.size()>maximum_pcm_ ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    return integrate(pcm) && scaling_.process(integrated_activity());
}
}
