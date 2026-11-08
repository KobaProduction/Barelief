// SPDX-License-Identifier: GPL-3.0-or-later
// Port of aubio@ad5cf975aed08cc4562dd008cf9f83b12b82ffb8,
// src/tempo/beattracking.c. Copyright (C) 2005-2009 Matthew Davies and Paul
// Brossier. C++/Float32 changes Copyright (C) 2026 Koba Production.
// This program is free software under GPL version3 or any later version.
// No warranty; see ../COPYING. This source is separate from the MIT DSP target.
#include <rhythm_reference_ports/context_beat_tracker.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
std::size_t source_maximum(std::span<const float> values) noexcept {
    // aubio fvec_max_elem starts at zero and replaces the index on equality.
    // Its last-tie rule also selects the phase fallback for silent histories.
    std::size_t selected=0;float maximum=0;
    for(std::size_t i=0;i<values.size();++i)
        if(!(maximum>values[i])){selected=i;maximum=values[i];}
    return selected;
}
float peak(std::span<const float> values) noexcept {
    const auto position=source_maximum(values);
    if(!position || position+1==values.size())return float(position);
    const float left=values[position-1],middle=values[position],right=values[position+1];
    const float denominator=left-2.f*middle+right;
    return denominator==0?float(position):float(position)+.5f*(left-right)/denominator;
}
bool valid(const ContextBeatTrackerConfig& c) noexcept {
    return c.window>=4 && c.window%4==0 && c.window<std::size_t(PTRDIFF_MAX)/8 &&
        std::isfinite(c.frame_rate) && c.frame_rate>0 && std::isfinite(60.f*c.frame_rate) &&
        std::isfinite(c.preferred_bpm) && c.preferred_bpm>0 && std::isfinite(c.context_variance) && c.context_variance>0 &&
        std::isfinite(c.min_period_frames) && c.min_period_frames>0;
}
}
bool context_detection_weights(ContextBeatTrackerConfig config,std::span<float> output) noexcept {
    if(!valid(config) || output.size()!=config.window)return false;
    const float preferred=60.f*config.frame_rate/config.preferred_bpm;
    for(std::size_t i=0;i<output.size();++i)
        output[i]=std::exp(std::log(2.f)/preferred*(float(i+1)-float(config.window+2)));
    return true;
}
bool PreparedContextBeatTracker::prepare(ContextBeatTrackerConfig config) {
    PreparedContextBeatTracker next;std::size_t bytes=0;
    if(!storage_bytes(config,bytes) || !next.configure(config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedContextBeatTracker::storage_bytes(ContextBeatTrackerConfig config,std::size_t& output) noexcept {
    if(!valid(config))return false;
    const float preferred=60.f*config.frame_rate/config.preferred_bpm;
    if(!std::isfinite(preferred) || preferred<=0 || preferred>=float(UINT32_MAX))return false;
    std::size_t bytes=0;const auto lag=config.window/4;
    using namespace dsp_kernels::prepared_storage;
    if(!add_array(bytes,config.window,3*sizeof(float)) || !add_array(bytes,lag,6*sizeof(float)) ||
       !add_array(bytes,1,sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedContextBeatTracker::workspace_bytes() const noexcept {
    return (rayleigh_.capacity()+context_weights_.capacity()+phase_weights_.capacity()+
        reversed_.capacity()+acf_.capacity()+comb_.capacity()+phase_response_.capacity()+offsets_.capacity())*sizeof(float);
}
bool PreparedContextBeatTracker::configure(ContextBeatTrackerConfig c) {
    if(!valid(c))return false;
    const float preferred=60.f*c.frame_rate/c.preferred_bpm;
    if(!std::isfinite(preferred) || preferred<=0 || preferred>=float(UINT32_MAX))return false;
    config_=c;ray_period_=float(std::uint32_t(preferred));
    const auto lag=c.window/4;
    allocate_buffers(c.window,lag);weights();reset();return true;
}
void PreparedContextBeatTracker::allocate_buffers(std::size_t window,std::size_t lag) {
    rayleigh_.resize(lag);context_weights_.resize(lag);phase_weights_.resize(2*lag);
    reversed_.resize(window);acf_.resize(window);
    comb_.resize(lag);phase_response_.resize(window);offsets_.resize(lag+1);
}
void PreparedContextBeatTracker::weights() noexcept {
    const float preferred=60.f*config_.frame_rate/config_.preferred_bpm;
    for(std::size_t i=0;i<rayleigh_.size();++i){
        const float x=float(i+1);
        rayleigh_[i]=x/(preferred*preferred)*std::exp(-x*x/(2.f*preferred*preferred));
    }
}
void PreparedContextBeatTracker::reset() noexcept {
    general_=1;context_=0;period_=0;previous_=0;older_=0;last_beat_=0;counter_=0;step_change_=false;time_signature_=0;
    std::fill(context_weights_.begin(),context_weights_.end(),0.f);
    std::fill(phase_weights_.begin(),phase_weights_.end(),0.f);
    std::fill(acf_.begin(),acf_.end(),0.f);std::fill(comb_.begin(),comb_.end(),0.f);
    std::fill(phase_response_.begin(),phase_response_.end(),0.f);std::fill(offsets_.begin(),offsets_.end(),0.f);
}
void PreparedContextBeatTracker::comb(unsigned harmonics,bool normalized,std::span<const float> weighting) noexcept {
    std::fill(comb_.begin(),comb_.end(),0.f);
    for(std::size_t lag=1;lag+1<comb_.size();++lag){
        for(unsigned harmonic=1;harmonic<=harmonics;++harmonic)
            for(unsigned offset=1;offset<2*harmonic;++offset){
                const auto at=lag*harmonic+offset-1;
                if(at<acf_.size())comb_[lag]+=acf_[at]/(normalized?float(2*harmonic-1):1.f);
            }
        comb_[lag]*=weighting[lag];
    }
}
bool PreparedContextBeatTracker::context_change() noexcept {
    if(counter_==0){
        step_change_=std::abs(context_-general_)>2.f*config_.context_variance;
        if(step_change_)counter_=3;
    }
    bool consistent=false;
    if(counter_==1 && step_change_){
        consistent=std::abs(2.f*general_-previous_-older_)<config_.context_variance;
        counter_=consistent?0:2;
    }else if(counter_>0)--counter_;
    older_=previous_;previous_=general_;return consistent;
}
float PreparedContextBeatTracker::lag_value(std::ptrdiff_t index) const noexcept {
    // Source meter code can address past the ACF. Use explicit zero extension,
    // retaining available evidence instead of undefined out-of-bounds access.
    return index>=0 && std::size_t(index)<acf_.size()?acf_[std::size_t(index)]:0;
}
unsigned PreparedContextBeatTracker::meter(float period) const noexcept {
    const auto lag=std::ptrdiff_t(period);if(lag<2)return 4;
    float three=0,four=0;
    for(std::ptrdiff_t offset=-2;offset<2;++offset){
        three+=lag_value(3*lag+offset);four+=lag_value(4*lag+offset);
        if(acf_.size()<=std::size_t(6*lag+2)){three+=lag_value(6*lag+offset);four+=lag_value(2*lag+offset);}
    }
    return three>four?3u:4u;
}
void PreparedContextBeatTracker::phase_weights(bool fresh) noexcept {
    if(fresh || !time_signature_ || float(step())<=last_beat_){
        std::fill(phase_weights_.begin(),phase_weights_.end(),1.f);return;
    }
    for(std::size_t i=0;i<phase_weights_.size();++i){
        const float difference=1.f+float(i)-float(step())+last_beat_;
        phase_weights_[i]=std::exp(-.5f*difference*difference/(period_/8.f));
    }
}
void PreparedContextBeatTracker::update_context() noexcept {
    if(context_!=0){comb(time_signature_,false,context_weights_);context_=peak(comb_);}
    const bool fresh=context_change();
    if(fresh)establish_context();
    else period_=time_signature_?context_:general_;
    phase_weights(fresh);
    while(period_>0 && period_<config_.min_period_frames)period_*=2.f;
}
void PreparedContextBeatTracker::establish_context() noexcept {
    context_=general_;time_signature_=meter(context_);period_=context_;
    for(std::size_t i=0;i<context_weights_.size();++i){
        const float distance=float(i+1)-context_;
        context_weights_[i]=std::exp(-.5f*distance*distance/(config_.context_variance*config_.context_variance));
    }
}
void PreparedContextBeatTracker::accumulate_phase_response(std::size_t repeats,std::size_t phases) noexcept {
    for(std::size_t at=0;at<phases;++at)
        for(std::size_t k=0;k<repeats;++k){
            const auto index=at+std::size_t(std::round(period_*float(k)));
            if(index<reversed_.size())phase_response_[at]+=reversed_[index];
        }
}
float PreparedContextBeatTracker::phase() noexcept {
    std::fill(phase_response_.begin(),phase_response_.end(),0.f);
    const auto repeats=std::size_t(std::floor(float(config_.window)/period_));
    const auto phases=std::min(std::size_t(std::ceil(period_)),phase_response_.size());
    accumulate_phase_response(repeats,phases);
    for(std::size_t at=0;at<phase_weights_.size();++at)phase_response_[at]*=phase_weights_[at];
    const auto maximum=source_maximum(phase_response_);
    return (maximum+1==phase_response_.size()?float(step())-last_beat_:peak(phase_response_))+1.f;
}
bool PreparedContextBeatTracker::schedule(float alignment,std::size_t& count) noexcept {
    float beat=period_-alignment;count=0;
    if(float(step())-last_beat_-alignment<-.4f*period_)beat+=period_;
    while(beat+period_<0)beat+=period_;
    if(beat>=0)offsets_[count++]=beat;
    while(beat+period_<=float(step())){
        beat+=period_;if(count>=offsets_.size())return false;offsets_[count++]=beat;
    }
    last_beat_=beat;return true;
}
float PreparedContextBeatTracker::confidence() const noexcept {
    if(context_<.5f || context_>=float(comb_.size()))return 0;
    float sum=0;
    for(float value:comb_)sum+=value;
    if(sum==0 || !std::isfinite(sum))return 0;
    const auto at=std::size_t(context_-.5f)+1;
    return confidence_at(at,sum);
}
float PreparedContextBeatTracker::confidence_at(std::size_t at,float sum) const noexcept {
    if(at>=comb_.size())return 0;
    if(float(at)==context_)return comb_[at]/sum;
    if(!at || at+1>=comb_.size())return 0;
    const float magnitude=comb_[at]-.25f*(comb_[at-1]-comb_[at+1])*(context_-float(at));
    return magnitude/sum;
}
bool PreparedContextBeatTracker::load_frame_inputs(std::span<const float> reversed,std::span<const float> acf,
    std::span<float> output) noexcept {
    if(!step() || reversed.size()!=config_.window || acf.size()!=config_.window || output.size()<maximum_beats())return false;
    const auto finite=[](float x){return std::isfinite(x);};
    if(!std::all_of(reversed.begin(),reversed.end(),finite) || !std::all_of(acf.begin(),acf.end(),finite))return false;
    std::copy(reversed.begin(),reversed.end(),reversed_.begin());std::copy(acf.begin(),acf.end(),acf_.begin());
    return true;
}
bool PreparedContextBeatTracker::process(std::span<const float> reversed,std::span<const float> acf,
    std::span<float> output,std::size_t& count) noexcept {
    if(!load_frame_inputs(reversed,acf,output))return false;
    comb(time_signature_?time_signature_:4u,true,rayleigh_);
    const auto maximum=source_maximum(comb_);
    general_=maximum && maximum+1<comb_.size()?peak(comb_):ray_period_;update_context();
    if(!std::isfinite(period_) || period_<0 || period_>float(config_.window))return false;
    std::size_t written=0;
    if(period_>0 && !schedule(phase(),written))return false;
    std::copy_n(offsets_.begin(),written,output.begin());count=written;return true;
}
}
