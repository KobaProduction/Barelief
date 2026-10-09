#pragma once
#include <dsp_kernels/kernel_support.h>
#include <dsp_kernels/scalar_reduce.h>
namespace dsp_kernels {
struct ReductionRequest {
    signal_contracts::NumericContract input,output;
    ScalarReduction operation;
    signal_contracts::PrecisionLoss precision_loss;
    signal_contracts::Rounding rounding;
    KernelLimits limits;
    signal_contracts::FrameLimits frame_limits;
};
struct ReductionSupport {NumericStatus status;KernelRequirements requirements;};
struct PreparedReduction {
    using Function=NumericResult (*)(const PreparedReduction&,const signal_contracts::NumericView&,
                                    signal_contracts::MutableNumericView&,std::span<std::byte>) noexcept;
    KernelContract input,output;
    ScalarReduction operation=ScalarReduction::MaximumAbsolute;
    signal_contracts::PrecisionLoss precision_loss=signal_contracts::PrecisionLoss::RejectInexact;
    signal_contracts::Rounding rounding=signal_contracts::Rounding::NearestTiesEven;
    KernelLimits limits{};
    signal_contracts::FrameLimits frame_limits{};
    KernelRequirements requirements;
    Function function=nullptr;
};
ReductionSupport reduction_support(const ReductionRequest&,const KernelCapabilities&) noexcept;
NumericStatus prepare_reduction(const ReductionRequest&,const KernelCapabilities&,PreparedReduction&) noexcept;
NumericResult reduce(const PreparedReduction&,const signal_contracts::NumericView&,
                     signal_contracts::MutableNumericView&,std::span<std::byte> scratch) noexcept;
}
