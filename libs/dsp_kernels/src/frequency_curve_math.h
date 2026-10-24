#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels::profile_curve_detail {
bool fit(FrequencyCurve&,double bin_hz);
double fitted_log_amplitude(const FrequencyCurve&,double hz) noexcept;
inline double regularized_gain(double measured,double fitted,double floor,double maximum) noexcept {
    if(measured<=0)return 1;
    const auto ratio=measured/std::max(floor,1e-300);
    const auto confidence=ratio>=1?1/(1+1/(ratio*ratio)):ratio*ratio/(1+ratio*ratio);
    const auto reciprocal=1/std::hypot(fitted,floor);
    return 1+confidence*(std::clamp(reciprocal,1.,maximum)-1);
}
}
