#include <dsp_kernels/complex_values.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>

// ScaledRatioFma finite-scalar operation order adapted from NumPy Developers,
// numpy@a3d0ef1df045b858e68115001fdabc930bb3b68c,
// numpy/_core/src/umath/loops_unary_complex.dispatch.c.src:simd_cabsolute_f32.
// Copyright (c) 2005-2025, NumPy Developers. BSD-3-Clause notice:
// packaging/licenses/numpy.txt. Inputs are finite by this API's contract.

namespace dsp_kernels {
namespace {
bool overlaps(std::span<const float> left,std::span<const float> right) noexcept {
    if(left.empty() || right.empty())return false;
    const std::less<const float*> before;
    return before(left.data(),right.data()+right.size()) && before(right.data(),left.data()+left.size());
}
bool valid(ComplexPolarOptions options) noexcept {
    return unsigned(options.zero_phase)<=unsigned(ZeroMagnitudePhase::Atan2) &&
        unsigned(options.magnitude_norm)<=unsigned(ComplexMagnitudeNorm::ScaledRatioFma);
}
float scaled_ratio_fma(float real,float imaginary) noexcept {
    const float a=std::abs(real),b=std::abs(imaginary);
    const float larger=std::max(a,b),smaller=std::min(a,b);
    if(larger==0)return 0.f;
    const float ratio=smaller/larger;
    return std::sqrt(std::fma(ratio,ratio,1.f))*larger;
}
float magnitude_value(float real,float imaginary,ComplexPolarOptions options) noexcept {
    if(options.real_axis)return std::abs(real);
    if(options.magnitude_norm==ComplexMagnitudeNorm::ScaledRatioFma)return scaled_ratio_fma(real,imaginary);
    return options.magnitude_norm==ComplexMagnitudeNorm::Hypot?std::hypot(real,imaginary):std::sqrt(real*real+imaginary*imaginary);
}
}
namespace {
float phase_value(float re,float im,ComplexPolarOptions options) noexcept {
    if(options.real_axis)return re<0?std::numbers::pi_v<float>:0.f;
    return (options.zero_phase==ZeroMagnitudePhase::Atan2 || re!=0 || im!=0)?std::atan2(im,re):0.f;
}
bool convert_polar_bin(std::span<const float> input,std::span<float> magnitude,std::span<float> phase,
    std::size_t i,ZeroMagnitudePhase zero,ComplexMagnitudeNorm norm,std::size_t real_fft_size) noexcept {
    const auto bins=input.size()/2;
    const bool real=real_fft_size && (!i || (!(real_fft_size%2) && i+1==bins));
    return complex_polar_value(input[2*i],input[2*i+1],magnitude.empty()?nullptr:&magnitude[i],
        phase.empty()?nullptr:&phase[i],{zero,norm,real});
}
}

bool complex_polar_value(float re,float im,float* magnitude,float* phase,ComplexPolarOptions options) noexcept {
    if((!magnitude && !phase) || !valid(options) || !std::isfinite(re) || !std::isfinite(im))return false;
    if(magnitude){
        const float value=magnitude_value(re,im,options);
        if(!std::isfinite(value))return false;
        *magnitude=value;
    }
    if(phase)*phase=phase_value(re,im,options);
    return true;
}
bool magnitude_power(float value,float exponent,float& output) noexcept {
    if(!std::isfinite(value) || value<0 || !std::isfinite(exponent))return false;
    const float result=exponent==1?value:std::pow(value,exponent);
    if(!std::isfinite(result))return false;
    output=result;return true;
}
bool magnitude_power(std::span<const float> values,float exponent,std::span<float> output) noexcept {
    if(values.size()!=output.size() || !std::isfinite(exponent) ||
       (values.data()!=output.data() && overlaps(values,output)))return false;
    for(std::size_t i=0;i<values.size();++i)if(!magnitude_power(values[i],exponent,output[i]))return false;
    return true;
}
bool cartesian_to_polar(std::span<const float> input,std::span<float> magnitude,std::span<float> phase,
    ZeroMagnitudePhase zero,ComplexMagnitudeNorm norm,std::size_t real_fft_size) noexcept {
    if(input.empty() || input.size()%2 || (magnitude.empty() && phase.empty()) || !valid({zero,norm,false}))return false;
    const auto bins=input.size()/2;
    if((!magnitude.empty() && magnitude.size()!=bins) || (!phase.empty() && phase.size()!=bins) ||
        (real_fft_size && bins!=real_fft_size/2+1))return false;
    if(overlaps(input,magnitude) || overlaps(input,phase) || overlaps(magnitude,phase))return false;
    for(std::size_t i=0;i<bins;++i){
        if(!convert_polar_bin(input,magnitude,phase,i,zero,norm,real_fft_size))return false;
    }
    return true;
}
bool expand_real_spectrum(std::span<const float> half,std::size_t n,std::span<float> full) noexcept {
    if(n<2 || n>SIZE_MAX/2 || half.size()!=2*(n/2+1) || full.size()!=2*n || overlaps(half,full))return false;
    if(!std::all_of(half.begin(),half.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(half.begin(),half.end(),full.begin());
    for(std::size_t i=n/2+1;i<n;++i){full[2*i]=half[2*(n-i)];full[2*i+1]=-half[2*(n-i)+1];}
    return true;
}
}
