#pragma once
#include <dsp_kernels/kernel_support.h>

namespace dsp_kernels {
struct ConversionRequest {
    signal_contracts::NumericContract input, output;
    signal_contracts::ConversionPolicy policy;
    KernelLimits limits;
    signal_contracts::FrameLimits frame_limits;
};
struct ConversionSupport { NumericStatus status; KernelRequirements requirements; };
struct PreparedConversion {
    using Function=NumericResult (*)(const PreparedConversion&,const signal_contracts::NumericView&,
        signal_contracts::MutableNumericView&,std::span<std::byte>) noexcept;
    KernelContract input,output;
    signal_contracts::ConversionPolicy policy;
    KernelLimits limits{};
    signal_contracts::FrameLimits frame_limits{};
    KernelRequirements requirements;
    Function function=nullptr;
};
ConversionSupport conversion_support(const ConversionRequest&,const KernelCapabilities&) noexcept;
NumericStatus prepare_conversion(const ConversionRequest&,const KernelCapabilities&,PreparedConversion& out) noexcept;
NumericResult convert(const PreparedConversion&,const signal_contracts::NumericView&,
    signal_contracts::MutableNumericView&,std::span<std::byte> scratch) noexcept;
}
