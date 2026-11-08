// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TempoTapDegara's probabilistic beat decoder at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/renewal_beat_decoder.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
namespace rhythm_reference_ports {
namespace {
constexpr float infinity=std::numeric_limits<float>::infinity();
bool finite_positive(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>0;});
}
}
bool PreparedRenewalBeatDecoder::prepare(std::size_t samples,std::size_t blocks,RenewalBeatConfig c) {
    std::size_t admitted=0;if(!storage_bytes(samples,blocks,c,admitted))return false;
    PreparedRenewalBeatDecoder next;if(!next.configure(samples,blocks,c))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedRenewalBeatDecoder::prepare_model_storage() {
    rate_=config_.frame_rate*float(config_.upsampling);resolution_=1.f/rate_;
    if(!std::isfinite(rate_) || !state_count(config_.maximum_period_s,maximum_states_) || maximum_blocks_>SIZE_MAX/maximum_states_)return false;
    const float radius=std::ceil(4.f*config_.sigma_ibi/resolution_);
    if(!std::isfinite(radius) || radius>=float((PTRDIFF_MAX-1)/2))return false;
    allocate(std::size_t(radius));gaussian();return true;
}
std::uint32_t PreparedRenewalBeatDecoder::reset_source(std::size_t block,float& best) const noexcept {
    std::uint32_t source=UINT32_MAX;
    for(std::size_t state=0;state<states_;++state){
        const float value=previous_[state]-reset_log_[block*maximum_states_+state];
        if(value<best){best=value;source=std::uint32_t(state);}
    }
    return source;
}

bool PreparedRenewalBeatDecoder::configure(std::size_t samples,std::size_t blocks,RenewalBeatConfig c) {
    if(!samples || !blocks || !c.upsampling || c.upsampling>4 || !std::isfinite(c.frame_rate) || c.frame_rate<=0)return false;
    if(!std::isfinite(c.maximum_period_s) || c.maximum_period_s<=0 || !std::isfinite(c.sigma_ibi) || c.sigma_ibi<=0)return false;
    if(!std::isfinite(c.transition_mix) || c.transition_mix<=0 || c.transition_mix>=1)return false;
    config_=c;maximum_samples_=samples;maximum_blocks_=blocks;
    return prepare_model_storage();
}
void PreparedRenewalBeatDecoder::allocate(std::size_t radius) {
    gaussian_.resize(2*radius+1);reset_log_.resize(maximum_blocks_*maximum_states_);advance_log_.resize(maximum_blocks_*maximum_states_);
    previous_.resize(maximum_states_);current_.resize(maximum_states_);onset_.resize(maximum_samples_);
    reset_from_.resize(maximum_samples_);path_.resize(maximum_samples_);beats_.resize(maximum_samples_);
}
bool PreparedRenewalBeatDecoder::state_count(float maximum_period,std::size_t& count) const noexcept {
    const float limit=maximum_period+3.f*config_.sigma_ibi;
    if(!std::isfinite(limit) || !std::isfinite(resolution_) || resolution_<=0 || limit/resolution_>=float(UINT32_MAX))return false;
    count=0;
    // The source enumerates Float32 IBI times inclusively, with repeated
    // addition. Neither ceil nor a single division preserves its end condition.
    for(float time=resolution_;time<=limit;){
        if(count==UINT32_MAX)return false;
        ++count;const float next=time+resolution_;if(next<=time)return false;time=next;
    }
    return count>=2;
}
void PreparedRenewalBeatDecoder::gaussian() noexcept {
    const auto center=std::ptrdiff_t(gaussian_.size()/2);
    const float amplitude=1.f/(config_.sigma_ibi*std::sqrt(2.f*std::numbers::pi_v<float>));
    const float denominator=-2.f*config_.sigma_ibi*config_.sigma_ibi,scale=.01f/float(config_.upsampling);
    for(std::size_t i=0;i<=std::size_t(center);++i){
        const float distance=float(std::ptrdiff_t(i)-center)*resolution_;
        const float value=amplitude*std::exp(distance*distance/denominator)*scale;
        gaussian_[i]=value<1e-12f?0:value;gaussian_[gaussian_.size()-1-i]=gaussian_[i];
    }
}
bool PreparedRenewalBeatDecoder::input(std::span<const float> values,std::span<const float> periods,std::span<const float> ends,std::span<std::uint8_t> output) const noexcept {
    if(!maximum_samples_ || values.size()>maximum_samples_ || values.size()!=output.size() || periods.empty() || periods.size()>maximum_blocks_ || periods.size()!=ends.size())return false;
    if(!finite_positive(periods) || !finite_positive(ends))return false;
    for(std::size_t i=0;i<periods.size();++i)if(periods[i]>config_.maximum_period_s || (i && ends[i]<=ends[i-1]))return false;
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>=0;});
}
bool PreparedRenewalBeatDecoder::normalize(std::span<const float> values) noexcept {
    const float maximum=values.empty()?0:*std::max_element(values.begin(),values.end());
    for(std::size_t i=0;i<values.size();++i){
        const float value=maximum>0?values[i]/maximum:0;
        onset_[i]=value==0?std::numeric_limits<float>::epsilon():value;
    }
    return true;
}
void PreparedRenewalBeatDecoder::model(float period,std::size_t block) noexcept {
    const auto shift=std::ptrdiff_t(gaussian_.size()/2)-std::ptrdiff_t(std::round(period/resolution_-1.f));
    float survival_log=0;
    for(std::size_t state=0;state<states_;++state){
        const auto gaussian_index=std::ptrdiff_t(state)+shift;
        const float pdf=gaussian_index>=0 && std::size_t(gaussian_index)<gaussian_.size()?gaussian_[std::size_t(gaussian_index)]:0;
        float probability=state==0?pdf:pdf>0?std::exp(std::log(pdf)-survival_log):0;
        probability=std::clamp(probability,0.f,1.f);
        reset_log_[block*maximum_states_+state]=config_.transition_mix*std::log(probability);
        const float advance=std::log1p(-probability);advance_log_[block*maximum_states_+state]=config_.transition_mix*advance;
        survival_log+=advance;
    }
}
bool PreparedRenewalBeatDecoder::models(std::span<const float> periods) noexcept {
    const float maximum=*std::max_element(periods.begin(),periods.end());
    if(!state_count(maximum,states_) || states_>maximum_states_)return false;
    for(std::size_t block=0;block<periods.size();++block)model(periods[block],block);
    return true;
}
void PreparedRenewalBeatDecoder::advance(float activation,std::size_t block,std::size_t frame) noexcept {
    const float beat_log=(1.f-config_.transition_mix)*std::log(.99f*activation);
    const float other_log=(1.f-config_.transition_mix)*std::log(1.f-.99f*activation);
    float best=infinity;const auto source=reset_source(block,best);
    current_[0]=best-beat_log;reset_from_[frame]=source;
    for(std::size_t state=1;state<states_;++state)
        current_[state]=previous_[state-1]-advance_log_[block*maximum_states_+state-1]-other_log;
    previous_.swap(current_);
}
bool PreparedRenewalBeatDecoder::backtrack(std::size_t samples) noexcept {
    const auto end=std::min_element(previous_.begin(),previous_.begin()+std::ptrdiff_t(states_));
    if(!std::isfinite(*end))return false;
    auto state=std::uint32_t(end-previous_.begin());
    for(std::size_t frame=samples;frame-->0;){
        path_[frame]=state;beats_[frame]=std::uint8_t(state==0);
        if(frame){state=state?state-1:reset_from_[frame];if(state>=states_)return false;}
    }
    return true;
}
bool PreparedRenewalBeatDecoder::decode(std::span<const float> periods,std::span<const float> ends,std::size_t samples) noexcept {
    std::fill(previous_.begin(),previous_.end(),infinity);previous_[0]=0;std::size_t block=0;
    for(std::size_t frame=0;frame<samples;++frame){
        advance(onset_[frame],block,frame);
        if(frame+1<samples){
            const float time=float(frame+1)*resolution_;
            for(std::size_t next=block+1;next<periods.size() && ends[next]<=time;++next)block=next;
        }
    }
    return backtrack(samples);
}
bool PreparedRenewalBeatDecoder::process(std::span<const float> onset,std::span<const float> periods,std::span<const float> ends,std::span<std::uint8_t> output) noexcept {
    result_samples_=0;
    if(maximum_samples_ && onset.empty() && output.empty())return true;
    if(!input(onset,periods,ends,output))return false;
    if(!normalize(onset) || !models(periods) || !decode(periods,ends,onset.size()))return false;
    std::copy_n(beats_.begin(),onset.size(),output.begin());result_samples_=onset.size();return true;
}
}
