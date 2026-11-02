#pragma once
#include <cstddef>
#include <span>
namespace dsp_kernels {
// Explicit circular left rotation: output[i]=input[(i+shift) mod N].
// Negative shifts rotate right. Exact in-place operation is supported;
// other overlapping spans are rejected. No windowing or Fourier is hidden.
bool rotate_samples(std::span<const float> input,std::span<float> output,std::ptrdiff_t shift) noexcept;
bool rotate_samples(std::span<const double> input,std::span<double> output,std::ptrdiff_t shift) noexcept;
// Embed one windowed frame into an explicitly sized Fourier buffer. Ordinary
// mode truncates/pads on the right. Zero-phase mode places the right half first,
// the left half last and zeros in between, retaining ordered overwrite when
// the FFT buffer is shorter than the frame. Either half must fit. Odd frames
// use floor(N/2) left and ceil(N/2) right, matching generic rotation semantics.
// Disjoint spans or exact equal-size in-place spans only; finite scalar data.
bool embed_fourier_frame(std::span<const float> frame,std::span<float> output,bool zero_phase=false) noexcept;
bool embed_fourier_frame(std::span<const double> frame,std::span<double> output,bool zero_phase=false) noexcept;
}
