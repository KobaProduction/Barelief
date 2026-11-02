#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
namespace dsp_kernels::detail {
inline bool finite_float_range(double value) noexcept {
    return std::isfinite(value) && std::abs(value)<=double(std::numeric_limits<float>::max());
}
inline bool finite_float_range(std::span<const double> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](double value){return finite_float_range(value);});
}
}
