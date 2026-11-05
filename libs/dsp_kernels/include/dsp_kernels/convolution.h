#pragma once
#include <cstddef>
#include <span>
namespace dsp_kernels {
enum class SameConvolutionCrop {Largest,Input};
// Zero-padded linear convolution with NumPy's one-dimensional 'same' crop.
// Default Largest crop yields max(input.size(),kernel.size()) values; empty kernel
// is an explicit bypass. Empty input yields no values. Disjoint input/output,
// finite values and finite results; no normalization or allocation.
// Input crop keeps input.size() values centered in the full convolution,
// as in a matrix same convolution along its time axis, even for longer kernels.
std::size_t same_convolution_size(std::size_t input,std::size_t kernel,
    SameConvolutionCrop=SameConvolutionCrop::Largest) noexcept;
bool convolve_same(std::span<const float> input,std::span<const float> kernel,
    std::span<float> output,SameConvolutionCrop=SameConvolutionCrop::Largest) noexcept;
// Reference Double evaluation follows NumPy's input-forward dot reduction;
// the established Float32 reduction remains unchanged.
bool convolve_same(std::span<const double> input,std::span<const double> kernel,
    std::span<double> output,SameConvolutionCrop=SameConvolutionCrop::Largest) noexcept;
}
