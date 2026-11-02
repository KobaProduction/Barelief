#pragma once
#include <cmath>
#include <algorithm>
#include <span>
#include <numbers>
#include <signal_contracts/window_policy.h>
#include <cstddef>
#include <array>
#include <string_view>
#include <limits>
namespace dsp_kernels {
namespace detail {
inline float cosine_series_value(std::size_t i,std::span<const float> coefficients,float period,bool mirror) noexcept {
    const float position=mirror?std::min(float(i),period-float(i)):float(i);
    const float angle=2.f*std::numbers::pi_v<float>*position/period;
    float value=coefficients[0];
    for(std::size_t k=1;k<coefficients.size();++k)value+=coefficients[k]*std::cos(float(k)*angle);
    return value;
}
inline double cosine_series_value(std::size_t i,std::span<const double> coefficients,double period,bool mirror) noexcept {
    const double position=mirror?std::min(double(i),period-double(i)):double(i);
    const double angle=2.*std::numbers::pi*position/period;
    double value=coefficients[0];
    for(std::size_t k=1;k<coefficients.size();++k)value+=coefficients[k]*std::cos(double(k)*angle);
    return value;
}
}
// Finite cosine-series window with explicit period in samples. Coefficient k
// multiplies cos(2*pi*k*i/period), including the DC coefficient at k=0.
// Mirror evaluates the nearer symmetric position, retaining exact symmetry.
inline bool cosine_sum_window(std::span<float> output,std::span<const float> coefficients,
                              float period,bool mirror=false) noexcept {
    if(output.empty() || coefficients.empty() || !std::isfinite(period) || period<=0)return false;
    if(!std::all_of(coefficients.begin(),coefficients.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<output.size();++i){
        const float value=detail::cosine_series_value(i,coefficients,period,mirror);
        if(!std::isfinite(value))return false;
        output[i]=value;
    }
    return true;
}

// Explicit source arithmetic: evaluate coefficients and trigonometry in
// Double, then store each completed coefficient in the caller's Float32 row.
inline bool cosine_sum_window(std::span<float> output,std::span<const double> coefficients,
                              double period,bool mirror=false) noexcept {
    if(output.empty() || coefficients.empty() || !std::isfinite(period) || period<=0)return false;
    if(!std::all_of(coefficients.begin(),coefficients.end(),[](double x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<output.size();++i){
        const double value=detail::cosine_series_value(i,coefficients,period,mirror);
        if(!std::isfinite(value) || std::abs(value)>double(std::numeric_limits<float>::max()))return false;
        output[i]=float(value);
    }
    return true;
}

// Source smoothing windows are symmetric and unnormalized; integer length 1
// is invalid in madmom smooth(), whereas length 0 disables smoothing.
inline bool symmetric_hamming_window(std::span<float> output) noexcept {
    if(output.empty())return true;
    if(output.size()==1)return false;
    constexpr std::array coefficients{.54f,-.46f};
    return cosine_sum_window(output,coefficients,float(output.size()-1),true);
}

using signal_contracts::WindowFunction;
using signal_contracts::window_function_names;
using signal_contracts::window_function_name;
using signal_contracts::parse_window_function;
template<typename Scalar> inline Scalar window_from_cosine_value(std::size_t i,std::size_t count,WindowFunction function,double cosine,bool periodic=false) noexcept {
    if(count<2 || function==WindowFunction::Rectangular)return 1;
    if(function==WindowFunction::Bartlett)return Scalar(1-std::abs(2*double(i)/(double(count)-(periodic?0:1))-1));
    const double c2=2*cosine*cosine-1,c3=4*cosine*cosine*cosine-3*cosine,c4=8*cosine*cosine*cosine*cosine-8*cosine*cosine+1;
    switch(function){
    case WindowFunction::Hann:return Scalar(.5-.5*cosine);
    case WindowFunction::Hamming:return Scalar(.54-.46*cosine);
    case WindowFunction::Blackman:return Scalar(.42-.5*cosine+.08*c2);
    case WindowFunction::BlackmanHarris:return Scalar(.35875-.48829*cosine+.14128*c2-.01168*c3);
    case WindowFunction::FlatTop:return Scalar(.21557895-.41663158*cosine+.277263158*c2-.083578947*c3+.006947368*c4);
    default:return 1;
    }
}
inline float window_from_cosine(std::size_t i,std::size_t count,WindowFunction function,double cosine,bool periodic=false) noexcept {
    return window_from_cosine_value<float>(i,count,function,cosine,periodic);
}
template<typename Scalar> inline Scalar window_coefficient_value(std::size_t i,std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    if(count<2 || function==WindowFunction::Rectangular)return 1;
    return window_from_cosine_value<Scalar>(i,count,function,std::cos(double(i)*6.2831853071795864769/(double(count)-(periodic?0:1))),periodic);
}
inline float window_coefficient(std::size_t i,std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    return window_coefficient_value<float>(i,count,function,periodic);
}
inline double window_sum(std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    double sum=0;for(std::size_t i=0;i<count;++i)sum+=window_coefficient(i,count,function,periodic);
    return sum;
}
inline double window_square_sum(std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    double sum=0;for(std::size_t i=0;i<count;++i){const double w=window_coefficient(i,count,function,periodic);sum+=w*w;}
    return sum;
}
inline double window_coherent_gain(std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    return count?window_sum(count,function,periodic)/double(count):0;
}
inline double window_power_gain(std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    return count?window_square_sum(count,function,periodic)/double(count):0;
}
}
