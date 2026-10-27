#include "curve_coefficients.h"
#include <dsp_kernels/window_math.h>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace desktop_analysis::offline {
namespace {
std::string float_literal(float value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text<<std::setprecision(std::numeric_limits<float>::max_digits10)<<value;
    auto result=text.str();
    if(result.find_first_of(".eE")==std::string::npos)result+=".0";
    return result+"f";
}
void emit_values(std::ostringstream& text,const std::vector<float>& gains) {
    for(std::size_t k=0;k<gains.size();++k) {
        if(k%8==0)text<<"\n    ";
        text<<float_literal(gains[k]);
        if(k+1<gains.size())text<<", ";
    }
}
}
bool frequency_coefficients_cpp_header(const dsp_kernels::FrequencyProfile& source,
    const dsp_kernels::ProfileCompatibility& target,std::string& header,std::string& error) {
    dsp_kernels::FrequencyProfile sampled;
    if(!source.curve){error="An analytic frequency function is required";return false;}
    if(!dsp_kernels::resample_frequency_profile(source,target,sampled,error))return false;
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text<<"#pragma once\n#include <array>\n\n"
        <<"// Barelief amplitude gain table, generated from saved frequency profile.\n"
        <<"// One-sided FFT bins k=0..N/2, f_k=k*sample_rate_hz/fft_size.\n"
        <<"// FFT input must use the same window and peak-amplitude calibration.\n"
        <<"// Source window: "<<dsp_kernels::window_function_name(target.window)<<"\n"
        <<"// Out-of-support bins and DC retain the profile-defined unity policy.\n"
        <<"namespace barelief_profile {\n"
        <<"inline constexpr double sample_rate_hz="
        <<std::setprecision(std::numeric_limits<double>::max_digits10)<<target.sample_rate<<";\n"
        <<"inline constexpr unsigned fft_size="<<target.fft_size<<";\n"
        <<"inline constexpr std::array<float, "<<sampled.gains.size()<<"> gains{{";
    emit_values(text,sampled.gains);
    text<<"\n}};\n}\n";
    header=text.str();return true;
}
}
