#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
namespace dsp_kernels {
enum class EvenMedian {Mean,Lower,HalfSum};
inline float even_median(float lower,float upper,EvenMedian policy) noexcept {
    if(policy==EvenMedian::Lower)return lower;
    return policy==EvenMedian::HalfSum?.5f*lower+.5f*upper:(lower+upper)/2.f;
}
// Finite Float32 median, with an explicit rule for two central values in an even
// population. Rearranges caller-owned scratch; no allocation. Empty/nonfinite
// populations return NaN without rearranging them. allow_nonfinite accepts
// infinities for source reductions; any NaN still propagates before reordering.
inline float median_in_place(std::span<float> values,EvenMedian policy=EvenMedian::Mean,bool allow_nonfinite=false) noexcept {
    if(unsigned(policy)>unsigned(EvenMedian::HalfSum) || values.empty() ||
       !std::all_of(values.begin(),values.end(),[allow_nonfinite](float x){return allow_nonfinite?!std::isnan(x):std::isfinite(x);}))
        return std::numeric_limits<float>::quiet_NaN();
    const auto middle=values.size()/2;
    std::nth_element(values.begin(),values.begin()+std::ptrdiff_t(middle),values.end());
    if(values.size()&1)return values[middle];
    return even_median(*std::max_element(values.begin(),values.begin()+std::ptrdiff_t(middle)),values[middle],policy);
}
}
