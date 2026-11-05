// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TempoTap at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/comb_period_phase.h>
#include <rhythm_reference_ports/periodic_comb.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
void hint_weights(std::span<float> weights,float period) noexcept {
    for(std::size_t i=0;i<weights.size();++i){
        const float deviation=float(i)-period;
        weights[i]=std::exp(-deviation*deviation/400.f);
    }
}
void rayleigh_weights(std::span<float> weights,std::size_t frames) noexcept {
    const float ray=48.f/512.f*float(frames),variance=ray*ray;
    for(std::size_t i=0;i<weights.size();++i){
        const float position=float(i+1);
        weights[i]=position/variance*std::exp(-position*position/(2.f*variance));
    }
}
}
bool PreparedCombPeriodPhase::geometry(CombPeriodPhaseConfig config) noexcept {
    if(config.number_frames<8 || config.number_frames>std::size_t(std::numeric_limits<int>::max()) ||
       !config.audio_frame_samples || !std::isfinite(config.sample_rate) || config.sample_rate<=0)return false;
    if(config.minimum_bpm<40 || config.minimum_bpm>180 || config.maximum_bpm<60 || config.maximum_bpm>250 ||
       config.maximum_bpm<config.minimum_bpm+20)return false;
    const float time=float(config.audio_frame_samples)/config.sample_rate;
    const float minimum=std::floor(60.f/time/float(config.maximum_bpm));
    const float maximum=std::ceil(60.f/time/float(config.minimum_bpm));
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(time) || time<=0 || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum>bound)return false;
    config_=config;frame_time_=time;minimum_lag_=minimum;maximum_lag_=maximum;return true;
}
bool PreparedCombPeriodPhase::prepare_weights(std::span<const float> hints) noexcept {
    if(hints.size()>2){
        if(!std::all_of(hints.begin(),hints.end(),[](float x){return std::isfinite(x);}))return false;
        const float period=(hints.back()-hints.front())/float(hints.size()-1)/frame_time_;
        if(!std::isfinite(period) || period<=minimum_lag_ || period>=maximum_lag_)return false;
        hint_weights(weights_,period);
        return true;
    }
    rayleigh_weights(weights_,config_.number_frames);
    return true;
}
bool PreparedCombPeriodPhase::prepare_detector() {
    InterpolatedPeakConfig peaks;peaks.range=float(comb_.size()-1);peaks.maximum=float(comb_.size());
    peaks.ordering=PeakOrdering::Amplitude;peaks.maximum_peaks=1;
    return detector_.prepare(std::max(comb_.size(),phase_response_.size()),peaks);
}
bool PreparedCombPeriodPhase::configure(CombPeriodPhaseConfig config,std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    if(!geometry(config))return false;
    dsp_kernels::SpectralAutocorrelationConfig acf;acf.normalization=dsp_kernels::CorrelationNormalization::Unbiased;
    if(!autocorrelation_.prepare(config.number_frames,acf,factory))return false;
    acf_.resize(config.number_frames);comb_.resize(config.number_frames/4);weights_.resize(comb_.size());
    phase_response_.resize(std::size_t(std::round(float(config.number_frames)/4.f)));
    return prepare_weights(hints) && prepare_detector();
}
bool PreparedCombPeriodPhase::prepare(CombPeriodPhaseConfig config,std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t admitted=0;if(!storage_bytes(config,hints,admitted,factory))return false;
    PreparedCombPeriodPhase next;
    if(!next.configure(config,hints,factory))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedCombPeriodPhase::estimate_period(std::span<const float> feature,float& period) noexcept {
    if(!autocorrelation_.process(feature,acf_) ||
       !harmonic_comb_response(acf_,weights_,1,comb_.size()-1,4,comb_) || !detector_.process(comb_))return false;
    const auto peaks=detector_.peaks();period=peaks.empty()?0:peaks.front().position;return true;
}
bool PreparedCombPeriodPhase::estimate_phase(std::span<const float> feature,float& period,float& phase) noexcept {
    phase=-1;
    if(period<=minimum_lag_ || period>=maximum_lag_){period=0;std::fill(phase_response_.begin(),phase_response_.end(),0.f);return true;}
    if(!periodic_phase_response(feature,period,4,phase_response_) || !detector_.process(phase_response_))return false;
    const auto peaks=detector_.peaks();phase=peaks.empty()?-1:peaks.front().position;
    while(phase>=period && period>minimum_lag_)phase-=period;
    return true;
}
bool PreparedCombPeriodPhase::process_feature(std::span<const float> feature,float& period,float& phase) noexcept {
    if(feature.size()!=config_.number_frames || acf_.empty())return false;
    float next_period=0,next_phase=-1;
    if(!estimate_period(feature,next_period) || !estimate_phase(feature,next_period,next_phase))return false;
    period=next_period;phase=next_phase;return true;
}
bool PreparedFeaturePeriodTracker::prepare(std::size_t features,std::size_t hop,CombPeriodPhaseConfig config,std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t admitted=0;if(!storage_bytes(features,hop,config,hints,admitted,factory))return false;
    if(!features || !hop || hop>config.number_frames)return false;
    PreparedFeaturePeriodTracker next;
    if(!next.configure(features,hop,config,hints,factory))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedFeaturePeriodTracker::configure(std::size_t features,std::size_t hop,CombPeriodPhaseConfig config,std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    if(!estimator_.prepare(config,hints,factory))return false;
    history_.resize(features);
    for(auto& history:history_)if(!history.prepare(config.number_frames))return false;
    periods_.resize(features);phases_.resize(features);evaluation_hop_=hop;return true;
}
void PreparedFeaturePeriodTracker::reset() noexcept {
    for(auto& history:history_)history.clear();
    pending_=0;ready_=false;
}
bool PreparedFeaturePeriodTracker::evaluate() noexcept {
    for(std::size_t feature=0;feature<history_.size();++feature)
        if(!estimator_.process_feature(history_[feature].values(),periods_[feature],phases_[feature]))return false;
    ready_=true;return true;
}
bool PreparedFeaturePeriodTracker::push(std::span<const float> frame) noexcept {
    ready_=false;
    if(history_.empty() || frame.size()!=history_.size() ||
       !std::all_of(frame.begin(),frame.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<frame.size();++i)history_[i].push(frame[i]);
    if(++pending_<evaluation_hop_)return true;
    pending_=0;return evaluate();
}
std::span<const float> PreparedFeaturePeriodTracker::periods() const noexcept {
    return ready_?std::span<const float>(periods_):std::span<const float>{};
}
std::span<const float> PreparedFeaturePeriodTracker::phases() const noexcept {
    return ready_?std::span<const float>(phases_):std::span<const float>{};
}
}
