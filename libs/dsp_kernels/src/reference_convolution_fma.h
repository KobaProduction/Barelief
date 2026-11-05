#pragma once
namespace dsp_kernels::detail {
using ReferenceConvolutionFma=double(*)(double,double,double) noexcept;
// IEEE binary64, round-to-nearest/even, gradual underflow; does not alter FENV.
ReferenceConvolutionFma select_reference_convolution_fma() noexcept;
}
