#pragma once
#include <cmath>
namespace dsp_kernels::detail {
template<typename Scalar> Scalar spectral_power(Scalar real,Scalar imaginary,Scalar divisor,Scalar exponent) noexcept {
    if(exponent==2 && divisor==1)return real*real+imaginary*imaginary;
    real/=divisor;imaginary/=divisor;
    return std::pow(std::sqrt(real*real+imaginary*imaginary),exponent);
}
template<typename Scalar> Scalar source_linear_segment(Scalar x0,Scalar x1,Scalar y0,Scalar y1,Scalar query) noexcept {
    return (y1-y0)/(x1-x0)*(query-x0)+y0;
}
}
