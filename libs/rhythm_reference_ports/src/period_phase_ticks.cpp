// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TempoTapTicks at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/period_phase_ticks.h>
#include <dsp_kernels/value_histogram.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedPeriodPhaseTicks::configure(std::size_t candidates,std::size_t matches,std::size_t ticks,PeriodPhaseTickConfig config) {
    const auto limit=std::size_t(std::numeric_limits<std::ptrdiff_t>::max());
    if(!candidates || candidates>limit-12 || !matches || !ticks || !config.evaluation_hop ||
       config.evaluation_hop>std::size_t(std::numeric_limits<int>::max()) || !config.audio_hop)return false;
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(config.sample_rate) || config.sample_rate<=0 ||
       !std::isfinite(config.maximum_candidate_frames) || config.maximum_candidate_frames<=0 ||
       config.maximum_candidate_frames>bound)return false;
    const float time=float(config.audio_hop)/config.sample_rate;
    if(!std::isfinite(time) || time<=0)return false;
    config_=config;maximum_candidates_=candidates;frame_time_=time;
    periods_.resize(candidates);phases_.resize(candidates+12);matches_.resize(matches);ticks_.resize(ticks);
    counts_.resize(std::size_t(config.maximum_candidate_frames/2.f+.5f)+1);return true;
}
bool PreparedPeriodPhaseTicks::prepare(std::size_t candidates,std::size_t matches,std::size_t ticks,PeriodPhaseTickConfig config) {
    std::size_t admitted=0;if(!storage_bytes(candidates,matches,ticks,config,admitted))return false;
    PreparedPeriodPhaseTicks next;
    if(!next.configure(candidates,matches,ticks,config))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedPeriodPhaseTicks::reset(bool clear_matches) noexcept {
    frames_=0;next_phase_=-1;tick_count_=0;status_=PeriodPhaseTickStatus::Unprocessed;
    if(clear_matches){match_count_=0;match_sum_=0;}
}
bool PreparedPeriodPhaseTicks::valid(std::span<const float> periods,std::span<const float> phases) const noexcept {
    if(!maximum_candidates_ || periods.size()!=phases.size() || periods.size()>maximum_candidates_)return false;
    const auto finite=[maximum=config_.maximum_candidate_frames](float x){return std::isfinite(x) && x<=maximum;};
    return std::all_of(periods.begin(),periods.end(),finite) && std::all_of(phases.begin(),phases.end(),finite);
}
std::size_t PreparedPeriodPhaseTicks::retain(std::span<const float> periods,std::span<const float> phases) noexcept {
    std::size_t count=0;
    for(std::size_t i=0;i<periods.size();++i)if(periods[i]!=0){
        periods_[count]=periods[i];phases_[count]=phases[i];++count;
    }
    return count;
}
std::size_t PreparedPeriodPhaseTicks::boost(std::span<const float> original,std::size_t count) noexcept {
    if(count>5)for(std::size_t i=0;i<4;++i){
        phases_[count++]=original[4];phases_[count++]=original[3];phases_[count++]=original[2];
    }
    return count;
}
bool PreparedPeriodPhaseTicks::closest(std::span<float> candidates,float& closest) noexcept {
    for(auto& value:candidates)value/=2.f;
    if(!dsp_kernels::rounded_bin_counts(candidates,counts_))return false;
    const auto selected=std::max_element(counts_.begin(),counts_.end())-counts_.begin();
    closest=float(selected)*2.f;
    for(auto& value:candidates)value*=2.f;
    return true;
}
bool PreparedPeriodPhaseTicks::period_vote(std::size_t count,float& period) noexcept {
    auto values=std::span(periods_).first(count);float closest_period=0;
    if(!closest(values,closest_period))return false;
    std::size_t additions=0;float sum=match_sum_;
    for(const float value:values)if(std::abs(closest_period-value)<2.f){++additions;sum+=value;}
    if(additions>matches_.size()-match_count_){status_=PeriodPhaseTickStatus::CapacityExceeded;return false;}
    if(!std::isfinite(sum))return false;
    for(const float value:values)if(std::abs(closest_period-value)<2.f)matches_[match_count_++]=value;
    match_sum_=sum;period=match_count_?sum/float(match_count_):closest_period;return true;
}
bool PreparedPeriodPhaseTicks::phase_vote(std::size_t count,float& phase) noexcept {
    auto values=std::span(phases_).first(count);float closest_phase=0;
    if(!closest(values,closest_phase))return false;
    float sum=0;std::size_t matches=0;
    for(const float value:values)if(std::abs(closest_phase-value)<2.f){sum+=value;++matches;}
    phase=matches?sum/float(matches):closest_phase;return std::isfinite(phase);
}
bool PreparedPeriodPhaseTicks::update_next_phase(float period,float phase) noexcept {
    const float rounded=std::round(phase),step=std::round(period);
    const float limit=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(rounded) || rounded<0 || rounded>limit || step>limit)return false;
    next_phase_=std::int32_t(rounded)%std::int32_t(config_.evaluation_hop);
    if(float(next_phase_)>period && step<1){status_=PeriodPhaseTickStatus::NonAdvancingPhase;return false;}
    while(float(next_phase_)>period)next_phase_-=std::int32_t(step);
    return true;
}
bool PreparedPeriodPhaseTicks::emit(float period,float phase) noexcept {
    if(period<=0)return true;
    const auto origin=std::int64_t(frames_)-std::int64_t(config_.evaluation_hop);
    while(phase<float(config_.evaluation_hop)){
        if(tick_count_==ticks_.size()){status_=PeriodPhaseTickStatus::CapacityExceeded;return false;}
        const float time=(float(origin)+phase)*frame_time_,next=phase+period;
        if(!std::isfinite(time) || !std::isfinite(next))return false;
        if(next<=phase){status_=PeriodPhaseTickStatus::NonAdvancingPhase;return false;}
        ticks_[tick_count_++]=time;phase=next;
    }
    return update_next_phase(period,phase);
}
bool PreparedPeriodPhaseTicks::process(std::span<const float> periods,std::span<const float> phases) noexcept {
    tick_count_=0;status_=PeriodPhaseTickStatus::InvalidInput;
    if(!valid(periods,phases) || frames_==std::uint64_t(std::numeric_limits<std::int64_t>::max()))return false;
    ++frames_;const auto count=retain(periods,phases);
    if(!count){match_count_=0;match_sum_=0;status_=PeriodPhaseTickStatus::NoCandidates;return true;}
    const auto phase_count=boost(phases,count);
    float period=0,phase=0;
    if(!period_vote(count,period) || !phase_vote(phase_count,phase) || !emit(period,phase))return false;
    status_=PeriodPhaseTickStatus::Ready;return true;
}
std::span<const float> PreparedPeriodPhaseTicks::ticks() const noexcept {
    return status_==PeriodPhaseTickStatus::Ready?std::span<const float>(ticks_).first(tick_count_):std::span<const float>{};
}
}
