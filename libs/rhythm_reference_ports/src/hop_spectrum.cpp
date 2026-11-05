// SPDX-License-Identifier: GPL-3.0-or-later
// aubio spectral/phasevoc.c, spectral/fft.c and mathutils.c analysis semantics,
// aubio/aubio@ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2014 Paul Brossier. GPL3 or later; no warranty, see ../COPYING.
#include <rhythm_reference_ports/hop_spectrum.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
bool valid_window(std::span<const float> supplied,std::size_t size,AnalysisWindowShape shape) noexcept {
    if(supplied.empty())return unsigned(shape)<=unsigned(AnalysisWindowShape::Parzen);
    return supplied.size()==size && std::all_of(supplied.begin(),supplied.end(),[](float value){return std::isfinite(value);});
}
bool prepare_window(std::span<float> coefficients,std::span<const float> supplied,AnalysisWindowShape shape) noexcept {
    if(supplied.empty())return analysis_window(coefficients,shape);
    if(supplied.size()!=coefficients.size() || !std::all_of(supplied.begin(),supplied.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(supplied.begin(),supplied.end(),coefficients.begin());return true;
}
}
bool PreparedHopSpectrum::prepare(HopSpectrumConfig config,std::span<const float> coefficients,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedHopSpectrum next;std::size_t bytes=0;
    if(!storage_bytes(config,coefficients,bytes,factory) || !next.configure(config,coefficients,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedHopSpectrum::storage_bytes(HopSpectrumConfig config,std::span<const float> supplied,std::size_t& output,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    const auto size=config.window_samples;
    if(size<2 || size>INT32_MAX || !config.hop_samples || config.hop_samples>size)return false;
    if(!valid_window(supplied,size,config.window))return false;
    dsp_kernels::TemporalWindowConfig window;window.length=size;window.hop=1;window.center=false;
    std::size_t bytes=0,history=0;
    if(!dsp_kernels::PreparedFourierTempogram::storage_bytes(window,supplied,size,false,bytes,factory) ||
       !ScalarHistory::storage_bytes(size,history) || !dsp_kernels::prepared_storage::add_bytes(bytes,history) ||
       !dsp_kernels::prepared_storage::add_array(bytes,size,sizeof(float)) ||
       !dsp_kernels::prepared_storage::add_array(bytes,2*(size/2+1),sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedHopSpectrum::workspace_bytes() const noexcept {
    return history_.workspace_bytes()+spectrum_.workspace_bytes()+(complex_.capacity()+coefficients_.capacity())*sizeof(float);
}
bool PreparedHopSpectrum::configure(HopSpectrumConfig c,std::span<const float> supplied,const dsp_kernels::FourierBackendFactory* factory) {
    if(c.window_samples<2 || c.window_samples>INT32_MAX || !c.hop_samples || c.hop_samples>c.window_samples)return false;
    config_=c;coefficients_.resize(c.window_samples);
    if(!prepare_window(coefficients_,supplied,c.window))return false;
    dsp_kernels::TemporalWindowConfig window;window.length=c.window_samples;window.hop=1;window.center=false;
    if(!history_.prepare(c.window_samples) || !spectrum_.prepare(window,coefficients_,0,false,factory))return false;
    complex_.resize(2*spectrum_.bins());return true;
}
void PreparedHopSpectrum::restart() noexcept {history_.clear();std::fill(complex_.begin(),complex_.end(),0.f);}
bool PreparedHopSpectrum::set_window(AnalysisWindowShape shape) noexcept {
    if(complex_.empty() || !analysis_window(coefficients_,shape) || !spectrum_.set_window_coefficients(coefficients_))return false;
    config_.window=shape;return true;
}
bool PreparedHopSpectrum::set_window_coefficients(std::span<const float> coefficients) noexcept {
    return !complex_.empty() && spectrum_.set_window_coefficients(coefficients);
}
bool PreparedHopSpectrum::append_hop(std::span<const float> pcm) noexcept {
    if(complex_.empty() || pcm.size()!=config_.hop_samples ||
        !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    for(float value:pcm)history_.push(value);
    return true;
}
bool PreparedHopSpectrum::analyze_current() noexcept {
    if(complex_.empty())return false;
    const auto rotation=std::ptrdiff_t(config_.window_samples-config_.window_samples/2);
    return spectrum_.process_at(history_.values(),0,complex_,rotation);
}
bool PreparedHopSpectrum::process_hop(std::span<const float> pcm,std::span<float> magnitude,std::span<float> phase) noexcept {
    if(complex_.empty() || pcm.size()!=config_.hop_samples || magnitude.size()!=bins() || phase.size()!=bins() ||
        !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    if(!append_hop(pcm) || !analyze_current())return false;
    return dsp_kernels::cartesian_to_polar(complex_,magnitude,phase,dsp_kernels::ZeroMagnitudePhase::Atan2,
        dsp_kernels::ComplexMagnitudeNorm::SquaredNorm,config_.window_samples);
}
}
