// SPDX-License-Identifier: GPL-3.0-or-later
// Port of aubio/src/onset/onset.c and silence calculation at
// aubio/aubio@ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2006-2013 Paul Brossier <piem@aubio.org> (onset.c).
// Copyright (C) 2003-2014 Paul Brossier <piem@aubio.org> (mathutils.c).
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/onset_decision.h>
#include <algorithm>
#include <cmath>
#include <limits>
namespace rhythm_reference_ports {
bool PreparedOnsetDecision::prepare(OnsetDecisionConfig config) noexcept {
    if(!config.hop_samples)return false;
    config_=config;prepared_=true;reset();return true;
}
void PreparedOnsetDecision::reset() noexcept {total_=0;last_=0;}
bool PreparedOnsetDecision::set_delay_samples(std::uint64_t delay) noexcept {
    if(!prepared_)return false;
    config_.delay_samples=delay;return true;
}
bool PreparedOnsetDecision::set_minimum_interval_samples(std::uint64_t interval) noexcept {
    if(!prepared_)return false;
    config_.minimum_interval_samples=interval;return true;
}
bool PreparedOnsetDecision::separated(std::uint64_t sample) const noexcept {
    return sample>last_ && sample-last_>config_.minimum_interval_samples;
}
bool PreparedOnsetDecision::candidate(float peak,bool silent,float& accepted) noexcept {
    accepted=0;if(silent)return true;
    const float offset=std::round(peak*float(config_.hop_samples));
    if(!std::isfinite(offset) || offset<0 || offset>=float(UINT64_MAX))return false;
    const auto shift=std::uint64_t(offset);
    if(shift>UINT64_MAX-total_)return false;
    const auto time=total_+shift;
    if(!separated(time) || (last_>0 && config_.delay_samples>time))return true;
    last_=std::max(config_.delay_samples,time);accepted=peak;return true;
}
void PreparedOnsetDecision::startup(bool silent,float& accepted) noexcept {
    accepted=0;
    if(total_>config_.delay_samples || silent || (total_ && !separated(total_)))return;
    accepted=float(config_.delay_samples/config_.hop_samples);last_=total_+config_.delay_samples;
}
bool PreparedOnsetDecision::process(float peak,bool silent,OnsetDecision& output) noexcept {
    if(!prepared_ || !std::isfinite(peak) || peak<0 || total_>UINT64_MAX-config_.hop_samples)return false;
    if(peak==0 && total_<=config_.delay_samples && config_.delay_samples>UINT64_MAX-total_)return false;
    float accepted=0;
    if(peak>0){if(!candidate(peak,silent,accepted))return false;}
    else startup(silent,accepted);
    output={accepted,last_sample(),accepted>0};
    total_+=config_.hop_samples;return true;
}
bool pcm_is_silent(std::span<const float> pcm,float threshold,bool& silent) noexcept {
    if(pcm.empty() || !std::isfinite(threshold))return false;
    float energy=0;
    for(float value:pcm){if(!std::isfinite(value))return false;energy+=value*value;}
    if(!std::isfinite(energy))return false;
    const float level=10.f*std::log10(energy/float(pcm.size()));
    silent=level<threshold;return true;
}
namespace {
void complex_recipe(OnsetRecipeParameters& p) noexcept {
    p.delay_hops=4.6f;p.threshold=.15f;p.whitening=true;p.compression=1;
}
void divergence_recipe(OnsetRecipeParameters& p,bool modified) noexcept {
    p.threshold=modified?.05f:.35f;p.whitening=true;p.compression=.02f;
}
void flux_recipe(OnsetRecipeParameters& p) noexcept {
    p.threshold=.18f;p.whitening=true;p.whitening_config.relax_time_s=100;
    p.whitening_config.floor=1;p.compression=10;
}
}
OnsetRecipeParameters onset_recipe_parameters(OnsetRecipe recipe) noexcept {
    OnsetRecipeParameters result;
    switch(recipe){
    case OnsetRecipe::FrequencyContent:result.threshold=.058f;result.compression=1;break;
    case OnsetRecipe::Complex:complex_recipe(result);break;
    case OnsetRecipe::ModifiedKullbackLeibler:divergence_recipe(result,true);break;
    case OnsetRecipe::KullbackLeibler:divergence_recipe(result,false);break;
    case OnsetRecipe::SpectralFlux:flux_recipe(result);break;
    case OnsetRecipe::OldDefault:result.minimum_interval_s=.02f;break;
    default:break;
    }
    return result;
}
}
