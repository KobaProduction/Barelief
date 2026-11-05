// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia Windowing recipes at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace rhythm_reference_ports {
namespace {
template<class Scalar> Scalar rounded(Scalar value,std::size_t decimals) noexcept {
    if(decimals>=6)return value;
    const Scalar scale=std::pow(Scalar(10),Scalar(decimals));
    return std::round(value*scale)/scale;
}
bool coefficients(SpectralWindowShape shape,std::array<double,4>& result) noexcept {
    switch(shape){
    case SpectralWindowShape::Hann:result={.5,-.5,0,0};return true;
    case SpectralWindowShape::Hamming:result={.53836,-.46164,0,0};return true;
    case SpectralWindowShape::CenteredHann:result={.5,.5,0,0};return true;
    case SpectralWindowShape::Rectangular:result={1,0,0,0};return true;
    case SpectralWindowShape::BlackmanHarris62:result={.44959,-.49364,.05677,0};return true;
    case SpectralWindowShape::BlackmanHarris70:result={.42323,-.49755,.07922,0};return true;
    case SpectralWindowShape::BlackmanHarris74:result={.40217,-.49703,.09892,-.00188};return true;
    case SpectralWindowShape::BlackmanHarris92:result={.35875,-.48829,.14128,-.01168};return true;
    default:return false;
    }
}
template<class Scalar> void triangular_window(std::span<float> output,Scalar size) noexcept {
    for(std::size_t i=0;i<output.size();++i)output[i]=float(Scalar(2)/size*(size/Scalar(2)-std::abs(float(Scalar(i)-(size-Scalar(1))/Scalar(2)))));
}
template<class Scalar> bool window_weights(SpectralWindowConfig config,std::array<Scalar,4>& weights) noexcept {
    std::array<double,4> constants{};
    if(!coefficients(config.shape,constants))return false;
    std::transform(constants.begin(),constants.end(),weights.begin(),[](double value){return Scalar(value);});
    const bool blackman=config.shape>=SpectralWindowShape::BlackmanHarris62;
    if(blackman || config.shape==SpectralWindowShape::Hamming)
        for(auto& weight:weights)weight=rounded(weight,config.constants_decimals);
    return true;
}
template<class Scalar> bool generate(std::span<float> output,SpectralWindowConfig config) noexcept {
    const Scalar size=Scalar(output.size())+Scalar(!config.symmetric);
    if(config.shape==SpectralWindowShape::Triangular){triangular_window(output,size);return true;}
    std::array<Scalar,4> weights{};
    if(!window_weights(config,weights))return false;
    const bool blackman=config.shape>=SpectralWindowShape::BlackmanHarris62;
    const Scalar period=size-Scalar(config.shape!=SpectralWindowShape::CenteredHann);
    return dsp_kernels::cosine_sum_window(output,weights,period,blackman);
}
}
bool normalize_spectral_window(std::span<float> values) noexcept {
    float sum=0;
    for(const float value:values)sum+=std::abs(value);
    if(!std::isfinite(sum))return false;
    if(sum>0)for(auto& value:values)value*=2.f/sum;
    return true;
}
bool spectral_window(std::span<float> output,SpectralWindowConfig config) noexcept {
    std::size_t bytes=0;if(!spectral_window_storage_bytes(output.size(),config,bytes))return false;
    const bool generated=config.precision==dsp_kernels::NumericalPrecision::DesktopReference?
        generate<double>(output,config):generate<float>(output,config);
    if(!generated)return false;
    return !config.normalized || normalize_spectral_window(output);
}
bool spectral_window_storage_bytes(std::size_t samples,SpectralWindowConfig config,std::size_t& bytes) noexcept {
    if(samples<2 || samples>std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) ||
       unsigned(config.shape)>unsigned(SpectralWindowShape::BlackmanHarris92) ||
       unsigned(config.precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    bytes=0;return true;
}
}
