// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogram at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/phase_tempo_pulse.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>

namespace rhythm_reference_ports {
namespace {
constexpr float pi=std::numbers::pi_v<float>,tau=2.f*pi;
bool unwrap(std::span<float> values) noexcept {
    if(!std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t i=1;i<values.size();++i){
        const float difference=values[i-1]-values[i];
        if(std::abs(difference)<=pi)continue;
        values[i]+=std::round(difference/tau)*tau;
        if(!std::isfinite(values[i]))return false;
    }
    return true;
}
bool positive(float value) noexcept {
    return std::isfinite(value) && value>0;
}
}
bool PreparedPhaseTempoPulse::prepare_filter_and_peaks(std::size_t samples) {
    const float maximum=30.f*config_.frame_rate;
    const float limit=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(maximum) || maximum<1 || maximum>limit)return false;
    if(!average_.prepare(std::size_t(maximum)))return false;
    auto novelty=std::size_t(.1f*config_.frame_rate);
    if(novelty%2)++novelty;
    novelty_delay_=float(novelty/2);
    InterpolatedPeakConfig peaks;peaks.range=float(samples-1);peaks.maximum=peaks.range;
    peaks.threshold=1e-4f;peaks.maximum_peaks=samples;
    return detector_.prepare(samples,peaks);
}
bool PreparedPhaseTempoPulse::configure(std::size_t frames,std::size_t bins,std::span<const float> window,PhaseTempoPulseConfig config) {
    if(!frames || bins<2 || window.empty() || !config.hop || frames>SIZE_MAX/config.hop)return false;
    if(!positive(config.frame_rate) || config.frame_rate<1 || !positive(config.frequency_step) ||
       !positive(config.frequency_step*60.f))return false;
    const auto samples=frames*config.hop;
    if(samples<2 || samples>std::size_t(std::numeric_limits<std::ptrdiff_t>::max()))return false;
    config_=config;maximum_frames_=frames;bpm_ratio_=config.frequency_step*60.f;
    if(!overlap_.prepare(frames,window,config.hop,false,dsp_kernels::OverlapNormalization::None,
                         dsp_kernels::OverlapSampleSelection::Positive) || !prepare_filter_and_peaks(samples))return false;
    phase_.resize(bins);frame_.resize(window.size());pulse_.resize(samples);ticks_.resize(samples);
    return true;
}
bool PreparedPhaseTempoPulse::prepare(std::size_t frames,std::size_t bins,std::span<const float> window,PhaseTempoPulseConfig config) {
    std::size_t bound=0;if(!storage_bytes(frames,bins,window,config,bound))return false;
    PreparedPhaseTempoPulse next;
    if(!next.configure(frames,bins,window,config) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedPhaseTempoPulse::reset() noexcept {
    samples_=0;tick_count_=0;diagnostic_frame_=false;overlap_.reset();average_.reset();
}
bool PreparedPhaseTempoPulse::load_phase(dsp_kernels::FloatMatrixSource source,std::size_t index) noexcept {
    return source.read && source.read(source.context,index,phase_) && unwrap(phase_);
}
bool PreparedPhaseTempoPulse::synthesize(std::size_t index,float bpm) noexcept {
    const float bin=bpm/bpm_ratio_;
    if(!std::isfinite(bin) || bin<0 || bin>float(phase_.size()-1))return false;
    const auto lower=std::size_t(bin),upper=std::min(lower+1,phase_.size()-1);
    const float phase=phase_[lower]+(bin-float(lower))*(phase_[upper]-phase_[lower]);
    const float frequency=bin*config_.frequency_step;
    if(frequency==0){diagnostic_frame_=false;return true;}
    for(std::size_t i=0;i<frame_.size();++i)
        frame_[i]=std::cos(tau*frequency*float(i)/config_.frame_rate+phase);
    diagnostic_frame_=true;
    return overlap_.add(index,frame_);
}
bool PreparedPhaseTempoPulse::reconstruct(dsp_kernels::FloatMatrixSource source,std::span<const float> bpms) noexcept {
    for(std::size_t i=0;i<bpms.size();++i){
        if(!std::isfinite(bpms[i]) || bpms[i]<0 || !load_phase(source,i) || !synthesize(i,bpms[i]))return false;
    }
    return overlap_.finish(std::span(pulse_).first(bpms.size()*config_.hop));
}
bool PreparedPhaseTempoPulse::smooth(std::span<const float> bpms,float& delay) noexcept {
    float maximum=1;
    for(const float bpm:bpms)maximum=std::max(maximum,bpm);
    const float length=30.f/maximum*config_.frame_rate;
    if(!std::isfinite(length) || length<1)return false;
    if(!average_.set_length(std::size_t(length)))return false;
    delay=length/2.f+novelty_delay_;
    auto values=std::span(pulse_).first(bpms.size()*config_.hop);
    return average_.process(values,values);
}
bool PreparedPhaseTempoPulse::detect(float delay) noexcept {
    const float range=float(samples_-1);
    if(!detector_.set_coordinates(range,0,range) ||
       !detector_.process(std::span(pulse_).first(samples_)))return false;
    tick_count_=0;
    for(const auto& peak:detector_.peaks())
        ticks_[tick_count_++]={std::max(0.f,peak.position-delay)/config_.frame_rate,peak.value};
    return true;
}
bool PreparedPhaseTempoPulse::process(dsp_kernels::FloatMatrixSource phases,std::span<const float> bpms) noexcept {
    reset();
    if(!maximum_frames_ || bpms.size()>maximum_frames_)return false;
    if(bpms.empty())return true;
    if(bpms.size()*config_.hop<2 || !reconstruct(phases,bpms))return false;
    float delay=0;
    if(!smooth(bpms,delay))return false;
    samples_=bpms.size()*config_.hop;
    if(!detect(delay)){samples_=0;tick_count_=0;return false;}
    return true;
}
}
