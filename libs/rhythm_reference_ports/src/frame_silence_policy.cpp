// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia FrameCutter/NoiseAdder/StartStopSilence at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/frame_silence_policy.h>
#include <dsp_kernels/filter_projection.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedFrameSilencePolicy::prepare(std::size_t size,FrameSilenceConfig config) {
    std::size_t admitted=0;if(!storage_bytes(size,config,admitted))return false;
    if(!size || !std::isfinite(config.threshold_power) || config.threshold_power<0 ||
       !std::isfinite(config.noise_trigger_power) || config.noise_trigger_power<0 ||
       !std::isfinite(config.noise_gain) || config.noise_gain<0 ||
       unsigned(config.boundaries)>unsigned(SilenceBoundaryRecipe::Streaming))return false;
    PreparedFrameSilencePolicy next;next.config_=config;next.generator_.seed(config.noise_seed);
    next.ones_.assign(size,1.f);next.scratch_.resize(size);if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedFrameSilencePolicy::reset() noexcept {
    bounds_={};was_silent_=false;finished_=false;
}
bool PreparedFrameSilencePolicy::mean_power(std::span<const float> frame,float& output) noexcept {
    float energy=0;
    if(!dsp_kernels::project_filter_weights(frame,ones_,energy,{false,true}))return false;
    output=energy/float(frame.size());return std::isfinite(output);
}
bool PreparedFrameSilencePolicy::add_noise(std::span<float> frame,std::size_t leading,std::size_t acquired) noexcept {
    std::fill(scratch_.begin(),scratch_.end(),0.f);
    std::copy_n(frame.begin(),acquired,scratch_.begin()+std::ptrdiff_t(leading));
    for(std::size_t i=0;i<frame.size();++i){
        const float uniform=config_.noise.next?config_.noise.next(config_.noise.context):
            float(generator_())/float(std::mt19937::max());
        if(!std::isfinite(uniform) || uniform<0 || uniform>1)return false;
        scratch_[i]+=config_.noise_gain*(uniform*2.f-1.f);
        if(!std::isfinite(scratch_[i]))return false;
    }
    std::copy(scratch_.begin(),scratch_.end(),frame.begin());return true;
}
void PreparedFrameSilencePolicy::standard(bool silent) noexcept {
    if(was_silent_){++bounds_.start;was_silent_=false;}
    if(silent && bounds_.stop==bounds_.start){bounds_.start=bounds_.frames;was_silent_=true;}
    if(!silent)bounds_.stop=bounds_.frames;
    bounds_.stop=std::max(bounds_.stop,bounds_.start);
}
void PreparedFrameSilencePolicy::streaming(bool silent) noexcept {
    if(silent && !bounds_.stop)bounds_.start=bounds_.frames;
    if(!silent)bounds_.stop=bounds_.frames;
}
bool PreparedFrameSilencePolicy::observe(bool silent) noexcept {
    if(bounds_.frames==std::uint64_t(std::numeric_limits<int>::max()))return false;
    if(config_.boundaries==SilenceBoundaryRecipe::Standard)standard(silent);
    else streaming(silent);
    ++bounds_.frames;return true;
}
bool PreparedFrameSilencePolicy::process(std::span<float> frame,std::size_t leading,std::size_t acquired) noexcept {
    if(ones_.empty() || finished_ || frame.size()!=ones_.size() ||
       leading>frame.size() || acquired>frame.size()-leading ||
       !std::all_of(frame.begin(),frame.end(),[](float x){return std::isfinite(x);}))return false;
    float power=0;
    if(!mean_power(frame,power))return false;
    if(config_.add_noise && power<config_.noise_trigger_power){
        if(!add_noise(frame,leading,acquired) || !mean_power(frame,power))return false;
    }
    return observe(power<config_.threshold_power);
}
bool PreparedFrameSilencePolicy::finish() noexcept {
    if(ones_.empty() || finished_)return false;
    if(config_.boundaries==SilenceBoundaryRecipe::Streaming){
        if(bounds_.start<bounds_.frames && bounds_.start && bounds_.start<bounds_.stop)++bounds_.start;
        bounds_.stop=std::max(bounds_.stop,bounds_.start);
    }
    finished_=true;return true;
}
}
