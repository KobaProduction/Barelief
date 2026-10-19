#pragma once
#include "parameter_sets.h"
namespace domain::detail {
inline constexpr ParameterDescriptor calibration_frontend[]{
        {"frontend_declared",C::Processing,V::Boolean,0,1,0,{}},
        {"sample_rate",C::Processing,V::Integer,1,4294967295.,32768,{}},
        {"fft_size",C::Processing,V::Integer,2,4294967295.,4096,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}},
        {"coherent_gain",C::Processing,V::Number,0,1e15,0,{}},
        {"power_gain",C::Processing,V::Number,0,1e15,0,{}},
        {"window_function",C::Processing,V::WindowFunction,0,0,0,"hann"},
        {"representation",C::Processing,V::String,0,0,0,"power"},
        {"normalize",C::Processing,V::Boolean,0,1,1,{}},
        {"positive_half",C::Processing,V::Boolean,0,1,1,{}},
        {"include_dc",C::Processing,V::Boolean,0,1,1,{}},
        {"include_nyquist",C::Processing,V::Boolean,0,1,1,{}},
        {"resampler_mode",C::Processing,V::String,0,0,0,"existing_fir"},
        {"resampler_quality",C::Processing,V::Integer,0,10,5,{}},
        {"channel_policy",C::Processing,V::String,0,0,0,"mean"},
        {"channel_index",C::Processing,V::Integer,0,65535,0,{}},
        {"analysis_interval_num",C::Processing,V::Integer,1,4294967295.,1,{}},
        {"analysis_interval_den",C::Processing,V::Integer,1,4294967295.,120,{}}
};
template<std::size_t N> constexpr auto with_calibration_frontend(const std::array<ParameterDescriptor,N>& operation) {
    std::array<ParameterDescriptor,N+std::size(calibration_frontend)> result{};
    std::copy(operation.begin(),operation.end(),result.begin());
    std::copy(std::begin(calibration_frontend),std::end(calibration_frontend),result.begin()+N);
    return result;
}
}
