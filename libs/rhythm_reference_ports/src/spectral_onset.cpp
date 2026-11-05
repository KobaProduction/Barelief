// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia OnsetDetection at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/spectral_onset.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedSpectralOnset::prepare_flux(std::size_t bins,bool positive) {
    dsp_kernels::SpectralDescriptorConfig config;config.half_wave=positive;
    config.aggregation=dsp_kernels::SpectralAggregation::Sum;config.compensated_sum=false;
    return descriptor_.prepare(bins,config);
}
bool PreparedSpectralOnset::prepare_mel(std::size_t bins,float rate) {
    if(rate<8000)return false;
    std::array<float,42> edges{};
    if(!warped_band_edges(edges,0,4000,TriangleWarp::Log10Mel))return false;
    TriangularSpectralBandConfig config;config.sample_rate=rate;
    config.weighting=TriangleWarp::Log10Mel;config.logarithm=false;
    scratch_.resize(40);
    return mel_.prepare(bins,edges,config) && prepare_flux(40,true);
}
bool PreparedSpectralOnset::configure(std::size_t bins,SpectralOnsetConfig config) {
    if(!bins || !std::isfinite(config.sample_rate) || config.sample_rate<=0 ||
       unsigned(config.method)>unsigned(SpectralOnsetMethod::Rms))return false;
    bins_=bins;method_=config.method;
    if(method_==SpectralOnsetMethod::Rms)return true;
    if(method_==SpectralOnsetMethod::MelFlux)return prepare_mel(bins,config.sample_rate);
    if(method_==SpectralOnsetMethod::Flux)return prepare_flux(bins,false);
    if(method_==SpectralOnsetMethod::Complex || method_==SpectralOnsetMethod::ComplexPhase){
        scratch_.resize(bins);
        return polar_.prepare(bins,method_==SpectralOnsetMethod::Complex?PolarNovelty::Complex:PolarNovelty::ComplexPhase);
    }
    dsp_kernels::SpectralDescriptorConfig hfc;hfc.method=dsp_kernels::SpectralDescriptor::HighFrequencyContent;
    hfc.bin_offset=0;hfc.frequency_step=bins>1?(config.sample_rate/2.f)/float(bins-1):0;
    hfc.aggregation=dsp_kernels::SpectralAggregation::Sum;hfc.compensated_sum=false;
    return descriptor_.prepare(bins,hfc);
}
bool PreparedSpectralOnset::prepare(std::size_t bins,SpectralOnsetConfig config) {
    PreparedSpectralOnset next;std::size_t bytes=0;
    if(!storage_bytes(bins,config,bytes) || !next.configure(bins,config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedSpectralOnset::reset() noexcept {
    first_=true;descriptor_.reset();polar_.reset();rms_.reset();
}
bool PreparedSpectralOnset::polar(std::span<const float> magnitude,std::span<const float> phase,float& output) noexcept {
    if(!polar_.process(magnitude,phase,scratch_))return false;
    const float value=std::accumulate(scratch_.begin(),scratch_.end(),0.f);
    if(!std::isfinite(value))return false;
    output=value;return true;
}
bool PreparedSpectralOnset::mel(std::span<const float> magnitude,float& output) noexcept {
    float value=0;
    if(!mel_.process(magnitude,scratch_) || !amplitude_decibels(scratch_,scratch_) ||
       !descriptor_.process_values(scratch_,value))return false;
    output=first_?0:value;first_=false;return true;
}
bool PreparedSpectralOnset::process(std::span<const float> magnitude,std::span<const float> phase,float& output) noexcept {
    if(!bins_ || magnitude.size()!=bins_ ||
       !std::all_of(magnitude.begin(),magnitude.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    if(method_==SpectralOnsetMethod::Complex || method_==SpectralOnsetMethod::ComplexPhase)return polar(magnitude,phase,output);
    if(method_==SpectralOnsetMethod::MelFlux)return mel(magnitude,output);
    if(method_==SpectralOnsetMethod::Rms)return rms_.process(magnitude,output);
    return descriptor_.process_values(magnitude,output);
}
}
