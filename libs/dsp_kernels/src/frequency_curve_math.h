#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels::profile_curve_detail {
bool fit(FrequencyCurve&,double bin_hz);
double fitted_log_amplitude(const FrequencyCurve&,double hz) noexcept;
inline double measurement_hz(const FrequencyCurve& c,std::size_t index,double step) {
    return c.measurement_hz.empty()?index*step:c.measurement_hz[index];
}
inline double measurement_weight(const FrequencyCurve& c,std::size_t index,double step) {
    if(c.measurement_hz.empty())return 1;
    const auto left=index?(c.measurement_hz[index-1]+c.measurement_hz[index])/2:c.minimum_hz;
    const auto right=index+1<c.measurement_hz.size()?(c.measurement_hz[index]+c.measurement_hz[index+1])/2:c.maximum_hz;
    return std::max(0.,std::min(right,c.maximum_hz)-std::max(left,c.minimum_hz))/step;
}
inline double regularized_gain(double measured,double fitted,double floor,double maximum) noexcept {
    if(measured<=0)return 1;
    const auto ratio=measured/std::max(floor,1e-300);
    const auto confidence=ratio>=1?1/(1+1/(ratio*ratio)):ratio*ratio/(1+ratio*ratio);
    const auto reciprocal=1/std::hypot(fitted,floor);
    return 1+confidence*(std::clamp(reciprocal,1.,maximum)-1);
}
}
