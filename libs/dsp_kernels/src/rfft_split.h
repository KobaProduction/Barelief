#pragma once
#include <cstddef>
namespace dsp_kernels::detail {
bool fft_input_max(const float* data, std::size_t n, float& maximum) noexcept;
bool fft_all_finite(const float* data, std::size_t n) noexcept;
// Caller guarantees distinct, non-overlapping instance/caller storage.
void fft_copy(const float* source, float* destination, std::size_t n) noexcept;
void rfft_split(float* data, const float* twiddles, std::size_t packed, bool scalar) noexcept;
const char* rfft_split_implementation() noexcept;
// Fused last radix2 stage + real reconstruction, from two FFT(N/4) outputs.
// half = N/4; twiddles contains 0.5*exp(-2*pi*i*k/N), 0 <= k <= N/4.
void rfft_mixed_split(float* output, const float* mixed, const float* twiddles, std::size_t half) noexcept;
}
