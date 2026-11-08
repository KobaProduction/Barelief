#pragma once
#include <limits>
#include <span>
namespace dsp_kernels {
enum class VectorNormalization {None,Maximum,Minimum,L0,Lp,Sum,StandardDeviation};
// Sum uses sequential Float32 accumulation. StandardDeviation divides by the
// population deviation without subtracting the mean. Zero scales leave values
// unchanged. Other modes retain the default tiny-scale threshold unless an
// explicit minimum_scale is supplied; zero admits every positive scale.
bool normalize_vector(std::span<float>,VectorNormalization,float exponent=2,
    float minimum_scale=std::numeric_limits<float>::min()) noexcept;
}
