#pragma once
namespace dsp_kernels::detail {
// Correctly rounded Float32 (1 + ratio*ratio) for finite 0 <= ratio <= 1.
// Requires IEEE binary32/binary64 round-to-nearest-even arithmetic; does not
// change the floating-point environment or replace the general fma operation.
float fused_square_plus_one(float ratio) noexcept;
}
