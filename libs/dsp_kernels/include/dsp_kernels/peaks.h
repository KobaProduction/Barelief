#pragma once
#include <algorithm>
#include <cmath>
#include <concepts>
#include <span>

namespace dsp_kernels {
template<std::floating_point Real>
inline Real quadratic_peak_offset(Real left,Real center,Real right) noexcept {
    if(!std::isfinite(left) || !std::isfinite(center) || !std::isfinite(right))return Real(0);
    const Real denominator=left-Real(2)*center+right;
    if(denominator>=0)return Real(0);
    return std::clamp(Real(.5)*(left-right)/denominator,Real(-.5),Real(.5));
}
inline double quadratic_peak_offset(double left,double center,double right) noexcept {
    return quadratic_peak_offset<double>(left,center,right);
}
// Strict-left/non-strict-right local maxima. The first plateau bin is the
// candidate; three-point positions reuse the shared quadratic estimator.
// Bounded caller storage, no allocation. Offsets are per input bin, while
// positions/heights use the returned prefix. Endpoints have zero offset.
bool interpolated_local_peaks(std::span<const float> input,float threshold,bool endpoints,
    std::span<float> positions,std::span<float> heights,std::span<float> offsets,std::size_t& count) noexcept;
}
