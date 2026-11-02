#pragma once
#include <span>
#include <cstddef>

namespace dsp_kernels {
// Dense row-major Float32 operations, no allocation. Factorization uses only
// the LOWER input triangle (matching lower=True); validates all input entries.
// Exact in-place factorization/solve is allowed. Failure may leave a prefix.
// A diagonal shift is explicit, applied BEFORE factorization.
bool cholesky_lower(std::span<const float> matrix,std::size_t dimension,
                     std::span<float> lower,float diagonal_shift=0) noexcept;
bool lower_triangular_solve(std::span<const float> lower,std::span<const float> rhs,
                            std::span<float> solution) noexcept;
}
