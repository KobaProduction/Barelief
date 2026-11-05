// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of OnsetDetection and OnsetDetectionGlobal at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/polar_novelty.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <utility>
namespace rhythm_reference_ports {
namespace {
bool nonnegative(std::span<const float> input) noexcept {
    return std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;});
}
}
bool PreparedPolarNovelty::storage_bytes(std::size_t bins,PolarNovelty method,std::size_t& bytes) noexcept {
    if(!bins || bins>SIZE_MAX/(4*sizeof(float)) || unsigned(method)>unsigned(PolarNovelty::BeatEmphasis))return false;
    bytes=4*bins*sizeof(float);return true;
}
bool PreparedPolarNovelty::prepare(std::size_t bins,PolarNovelty method) {
    std::size_t bytes=0;if(!storage_bytes(bins,method,bytes))return false;
    PreparedPolarNovelty next;next.method_=method;next.magnitude_.resize(bins);
    next.phase_.resize(bins);next.older_.resize(bins);next.staging_.resize(bins);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedPolarNovelty::reset() noexcept {
    std::fill(magnitude_.begin(),magnitude_.end(),0);
    std::fill(phase_.begin(),phase_.end(),0);std::fill(older_.begin(),older_.end(),0);
}
float PreparedPolarNovelty::distance(std::size_t bin,float magnitude,float phase) const noexcept {
    const float sign=method_==PolarNovelty::Complex?-1.f:1.f;
    float target=2.f*phase_[bin]+sign*older_[bin];
    if(method_==PolarNovelty::ComplexPhase){
        const float value=2.f*magnitude*std::sin((phase-target)*.5f);return value*value;
    }
    // Both named Essentia source methods use Double M_PI operands and the
    // floor-quotient essentia::fmod helper, then store the target once as Real.
    constexpr double pi=std::numbers::pi_v<double>,period=-2.*pi;
    const double shifted=double(target)+pi;
    target=float(shifted-std::floor(shifted/period)*period+pi);
    const auto delta=magnitude_[bin]-std::polar(magnitude,phase-target);
    return method_==PolarNovelty::Complex?std::abs(delta):std::norm(delta);
}
bool PreparedPolarNovelty::process(std::span<const float> magnitude,std::span<const float> phase,std::span<float> output) noexcept {
    if(magnitude_.empty() || magnitude.size()!=magnitude_.size() || phase.size()!=magnitude.size() || output.size()!=magnitude.size())return false;
    if(!nonnegative(magnitude) || !std::all_of(phase.begin(),phase.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<magnitude.size();++i){
        staging_[i]=distance(i,magnitude[i],phase[i]);if(!std::isfinite(staging_[i]))return false;
    }
    older_=phase_;std::copy(phase.begin(),phase.end(),phase_.begin());std::copy(magnitude.begin(),magnitude.end(),magnitude_.begin());
    std::copy(staging_.begin(),staging_.end(),output.begin());return true;
}
bool PreparedSpectralRmsFlux::process(std::span<const float> magnitude,float& output) noexcept {
    if(magnitude.empty() || !nonnegative(magnitude))return false;
    float squared=0;
    for(float x:magnitude)squared+=x*x;
    const float rms=std::sqrt(squared)/float(magnitude.size());
    if(!std::isfinite(rms))return false;
    output=first_?0:std::max(0.f,rms-previous_);previous_=rms;first_=false;return true;
}
bool amplitude_decibels(std::span<const float> input,std::span<float> output) noexcept {
    if(input.size()!=output.size() || !nonnegative(input))return false;
    for(std::size_t i=0;i<input.size();++i)output[i]=input[i]<1e-10f?-200.f:20.f*std::log10(input[i]);
    return true;
}
}
