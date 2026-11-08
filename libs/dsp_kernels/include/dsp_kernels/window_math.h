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
#include <type_traits>
namespace dsp_kernels {
namespace detail {
inline float cosine_series_value(std::size_t i,std::span<const float> coefficients,float period,bool mirror) noexcept {
    const float position=mirror?std::min(float(i),period-float(i)):float(i);
    const float angle=2.f*std::numbers::pi_v<float>*position/period;
    float value=coefficients[0];
    for(std::size_t k=1;k<coefficients.size();++k)value+=coefficients[k]*std::cos(float(k)*angle);
    return value;
}
inline double cosine_series_value(std::size_t i,std::span<const double> coefficients,double period,bool mirror,double origin=0) noexcept {
    const double position=(mirror?std::min(double(i),period-double(i)):double(i))+origin;
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

inline bool cosine_sum_window(std::span<double> output,std::span<const double> coefficients,
                              double period,bool mirror=false,double origin=0) noexcept {
    if(output.empty() || coefficients.empty() || !std::isfinite(period) || period<=0 || !std::isfinite(origin))return false;
    if(!std::all_of(coefficients.begin(),coefficients.end(),[](double x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<output.size();++i){
        const double value=detail::cosine_series_value(i,coefficients,period,mirror,origin);
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
inline bool symmetric_hamming_window(std::span<double> output) noexcept {
    if(output.empty())return true;
    if(output.size()==1)return false;
    constexpr std::array coefficients{.54,.46};
    const double period=double(output.size()-1);
    return cosine_sum_window(output,coefficients,period,false,-period/2.);
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
template<typename Scalar> inline Scalar direct_window_coefficient(std::size_t i,std::size_t count,WindowFunction function,bool periodic,double pi) noexcept {
    if(count<2 || function==WindowFunction::Rectangular)return 1;
    const double position=double(i)/(double(count)-(periodic?0:1));
    const double cosine=std::cos(2*pi*position);
    if(function==WindowFunction::Hann)return Scalar(.5*(1-cosine));
    return window_from_cosine_value<Scalar>(i,count,function,cosine,periodic);
}
inline float window_coefficient(std::size_t i,std::size_t count,WindowFunction function,bool periodic=false) noexcept {
    return window_coefficient_value<float>(i,count,function,periodic);
}
template<class Scalar> inline std::array<Scalar,5> symmetric_cosine_terms(WindowFunction function) noexcept {
    switch(function){
    case WindowFunction::Hann:return {Scalar(.5),Scalar(.5),0,0,0};
    case WindowFunction::Hamming:return {Scalar(.54),Scalar(.46),0,0,0};
    case WindowFunction::Blackman:return {Scalar(.42),Scalar(.5),Scalar(.08),0,0};
    case WindowFunction::BlackmanHarris:return {Scalar(.35875),Scalar(.48829),Scalar(.14128),Scalar(.01168),0};
    case WindowFunction::FlatTop:return {Scalar(.21557895),Scalar(.41663158),Scalar(.277263158),Scalar(.083578947),Scalar(.006947368)};
    default:return {Scalar(1),0,0,0,0};
    }
}
inline std::size_t symmetric_cosine_term_count(WindowFunction function) noexcept {
    switch(function){
    case WindowFunction::FlatTop:return 5;
    case WindowFunction::BlackmanHarris:return 4;
    case WindowFunction::Blackman:return 3;
    default:return 2;
    }
}
inline bool evaluate_symmetric_cosine_series(std::span<float> output,std::span<float> coefficients,bool periodic) noexcept {
    for(std::size_t i=1;i<coefficients.size();i+=2)coefficients[i]=-coefficients[i];
    return cosine_sum_window(output,coefficients,float(output.size()-(periodic?0:1)),true);
}
inline bool evaluate_symmetric_cosine_series(std::span<double> output,std::span<double> coefficients,bool periodic) noexcept {
    const double period=double(output.size())-(periodic?0:1);
    return cosine_sum_window(output,coefficients,period,false,-period/2.);
}

template<class Scalar> inline bool symmetric_cosine_window(std::span<Scalar> output,WindowFunction function,bool periodic=false) noexcept {
    if(output.empty())return false;
    if(output.size()==1 || function==WindowFunction::Rectangular || function==WindowFunction::Bartlett){
        for(std::size_t i=0;i<output.size();++i)output[i]=window_coefficient_value<Scalar>(i,output.size(),function,periodic);
        return true;
    }
    auto terms=symmetric_cosine_terms<Scalar>(function);
    return evaluate_symmetric_cosine_series(output,std::span(terms).first(symmetric_cosine_term_count(function)),periodic);
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
