#pragma once
#include <cmath>
#include <signal_contracts/window_policy.h>
#include <cstddef>
#include <array>
#include <string_view>
namespace dsp_kernels {
using signal_contracts::WindowFunction;
using signal_contracts::window_function_names;
using signal_contracts::window_function_name;
using signal_contracts::parse_window_function;
inline float window_from_cosine(std::size_t i,std::size_t count,WindowFunction function,double cosine) noexcept {
    if(count<2 || function==WindowFunction::Rectangular)return 1;
    if(function==WindowFunction::Bartlett)return float(1-std::abs(2*double(i)/double(count-1)-1));
    const double c2=2*cosine*cosine-1,c3=4*cosine*cosine*cosine-3*cosine,c4=8*cosine*cosine*cosine*cosine-8*cosine*cosine+1;
    switch(function){
    case WindowFunction::Hann:return float(.5-.5*cosine);
    case WindowFunction::Hamming:return float(.54-.46*cosine);
    case WindowFunction::Blackman:return float(.42-.5*cosine+.08*c2);
    case WindowFunction::BlackmanHarris:return float(.35875-.48829*cosine+.14128*c2-.01168*c3);
    case WindowFunction::FlatTop:return float(.21557895-.41663158*cosine+.277263158*c2-.083578947*c3+.006947368*c4);
    default:return 1;
    }
}
inline float window_coefficient(std::size_t i,std::size_t count,WindowFunction function) noexcept {
    if(count<2 || function==WindowFunction::Rectangular)return 1;
    return window_from_cosine(i,count,function,std::cos(double(i)*6.2831853071795864769/double(count-1)));
}
inline double window_sum(std::size_t count,WindowFunction function) noexcept {
    double sum=0;for(std::size_t i=0;i<count;++i)sum+=window_coefficient(i,count,function);
    return sum;
}
inline double window_square_sum(std::size_t count,WindowFunction function) noexcept {
    double sum=0;for(std::size_t i=0;i<count;++i){const double w=window_coefficient(i,count,function);sum+=w*w;}
    return sum;
}
inline double window_coherent_gain(std::size_t count,WindowFunction function) noexcept {
    return count?window_sum(count,function)/double(count):0;
}
inline double window_power_gain(std::size_t count,WindowFunction function) noexcept {
    return count?window_square_sum(count,function)/double(count):0;
}
}
