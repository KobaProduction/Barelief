#pragma once
#include <dsp_kernels/filter_projection.h>
#include <cstddef>
#include <span>
namespace dsp_kernels {
struct DiscreteTriangleGeometry {std::size_t center=0;FilterBandSupport nonzero;};
// Shared bin-index triangular filter. [first,stop), center included on the
// falling side; source width<2 collapses to one unit bin. Optional Float32
// sum-area normalization. Whole coefficient row is initialized, including zeros.
bool discrete_triangle_filter(std::size_t first,std::size_t center,std::size_t stop,
    bool normalize,std::span<float> weights,DiscreteTriangleGeometry&) noexcept;
}
