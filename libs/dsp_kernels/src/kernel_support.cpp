#include <dsp_kernels/kernel_support.h>
#include <dsp_kernels/dsp.h>
#include "numeric_arithmetic.h"

namespace dsp_kernels {
bool declared_ieee64_environment_supported() noexcept {
    return numeric_detail::declared_ieee64_environment_available;
}
std::span<const KernelSupport> kernel_support_table() noexcept {
    using signal_contracts::DType;using signal_contracts::ValueDomain;
    static constexpr KernelSupport table[]{
#define D_NUMERIC(T) {KernelKind::Converter,DType::T,ValueDomain::AnyIeee,0,false}, {KernelKind::Reduction,DType::T,ValueDomain::FiniteOnly,0,false}
        D_NUMERIC(Int8),D_NUMERIC(UInt8),D_NUMERIC(Int16),D_NUMERIC(UInt16),D_NUMERIC(Int32),
        D_NUMERIC(UInt32),D_NUMERIC(Int64),D_NUMERIC(UInt64),D_NUMERIC(Float32),D_NUMERIC(Float64),
#undef D_NUMERIC
        {KernelKind::WindowCoefficients,DType::Float32,ValueDomain::FiniteOnly,0,false},
        {KernelKind::FFT,DType::Float32,ValueDomain::FiniteOnly,max_fft_samples,false},
        {KernelKind::Bark,DType::Float32,ValueDomain::FiniteOnly,0,false},
        {KernelKind::Mel,DType::Float32,ValueDomain::FiniteOnly,max_fft_samples,false},
        {KernelKind::StereoProcessor,DType::Float32,ValueDomain::FiniteOnly,max_fft_samples,false}
    };
    return table;
}
}
