// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia SingleBeatLoudness/EnergyBandRatio at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/beat_segment_energy.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool beat_energy_parameters(BeatSegmentEnergyConfig config,float bound) noexcept {
    return std::isfinite(config.sample_rate) && config.sample_rate>=1 && config.sample_rate<=bound &&
        std::isfinite(config.search_seconds) && config.search_seconds>0 &&
        std::isfinite(config.energy_seconds) && config.energy_seconds>0;
}
bool beat_energy_lengths(BeatSegmentEnergyConfig config,float rate,float bound,std::size_t& search_samples,std::size_t& energy_samples) noexcept {
    const float search=config.search_seconds*rate,length=config.energy_seconds*rate;
    if(!std::isfinite(search) || !std::isfinite(length) || search>bound || length>bound || search<1 || length<1)return false;
    search_samples=std::size_t(search);energy_samples=std::size_t(length);
    if(energy_samples%2)++energy_samples;
    return energy_samples<=search_samples && search_samples<=std::size_t(std::numeric_limits<int>::max())-energy_samples;
}
}
bool PreparedBeatSegmentEnergy::geometry(BeatSegmentEnergyConfig config) noexcept {
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!beat_energy_parameters(config,bound))return false;
    const float rate=std::trunc(config.sample_rate);
    if(!beat_energy_lengths(config,rate,bound,search_samples_,energy_samples_))return false;
    if(config.alignment!=BeatOnsetAlignment::PeakSample && config.alignment!=BeatOnsetAlignment::MaximumWindowEnergy)return false;
    effective_rate_=rate;config_=config;return true;
}
bool PreparedBeatSegmentEnergy::prepare_spectrum(const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig window;window.length=energy_samples_;window.hop=energy_samples_;
    window.center=false;window.periodic=false;
    std::vector<float> coefficients(energy_samples_);
    SpectralWindowConfig shape;shape.precision=config_.window_precision;shape.shape=SpectralWindowShape::BlackmanHarris62;
    if(!spectral_window(coefficients,shape) || !spectrum_.prepare(window,coefficients,0,false,factory))return false;
    ones_.assign(energy_samples_,1.f);complex_.resize(2*spectrum_.bins());magnitude_.resize(spectrum_.bins());return true;
}
bool PreparedBeatSegmentEnergy::configure(BeatSegmentEnergyConfig config,std::span<const float> borders,const dsp_kernels::FourierBackendFactory* factory) {
    if(!geometry(config) || !prepare_spectrum(factory) || !bands_.prepare(spectrum_.bins(),effective_rate_,borders))return false;
    ratios_.resize(bands_.bands());return true;
}
bool PreparedBeatSegmentEnergy::prepare(BeatSegmentEnergyConfig config,std::span<const float> borders,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(config,borders,bound,factory))return false;
    PreparedBeatSegmentEnergy next;
    if(!next.configure(config,borders,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatSegmentEnergy::align(std::span<const float> input,std::size_t& selected) const noexcept {
    float maximum=0;selected=0;
    for(std::size_t i=0;i<search_samples_;++i){
        float value=input[i]*input[i];
        if(config_.alignment==BeatOnsetAlignment::MaximumWindowEnergy &&
           !dsp_kernels::project_filter_weights(input.subspan(i,energy_samples_),ones_,value,{false,true}))return false;
        if(!std::isfinite(value))return false;
        if(value>maximum){maximum=value;selected=i;}
    }
    return true;
}
bool PreparedBeatSegmentEnergy::process(std::span<const float> input,BeatSegmentEnergyResult& output,std::span<float> ratios) noexcept {
    if(!energy_samples_ || input.size()<minimum_segment_samples() || ratios.size()!=ratios_.size() ||
       !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    BeatSegmentEnergyResult next;
    if(!align(input,next.onset_sample) ||
       !spectrum_.process_frame(input.subspan(next.onset_sample,energy_samples_),0,complex_) ||
       !dsp_kernels::cartesian_to_polar(complex_,magnitude_,{}) || !bands_.process(magnitude_,next.energy,ratios_))return false;
    std::copy(ratios_.begin(),ratios_.end(),ratios.begin());output=next;return true;
}
}
