#pragma once
#include <domain/processing_contracts.h>
#include <dsp_kernels/prepared_resampler.h>
#include <memory>
#include <signal_contracts/data_frame.h>

namespace dataflow_runtime { struct NumericRuntimeProfile; }
namespace dataflow_runtime::detail {
struct PlanNode;struct State;struct OperationKernelState;
// Cold-owned prepared operation state; numeric, signal and music handlers are separate.
class PreparedOperationKernel {
public:
    PreparedOperationKernel();~PreparedOperationKernel();
    PreparedOperationKernel(const PreparedOperationKernel&);PreparedOperationKernel& operator=(const PreparedOperationKernel&);
    PreparedOperationKernel(PreparedOperationKernel&&) noexcept;PreparedOperationKernel& operator=(PreparedOperationKernel&&) noexcept;
    void reset() noexcept;
    void invalidate_product_lane(std::size_t lane) noexcept;
    bool copy_state_from(const PreparedOperationKernel&) noexcept;
private:
    std::unique_ptr<OperationKernelState> value_;
    friend bool music_support(const domain::ResolvedProcessingNode&,domain::ProcessingResources&,const NumericRuntimeProfile&) noexcept;
    friend signal_contracts::Status prepare_music(PlanNode&,State&,const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
    friend signal_contracts::Status execute_music(const PlanNode&,State&,const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
    friend signal_contracts::Status accept_fourier(const PlanNode&,State&,std::size_t,const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
};
bool music_support(const domain::ResolvedProcessingNode&,domain::ProcessingResources&,const NumericRuntimeProfile&) noexcept;
signal_contracts::Status prepare_music(PlanNode&,State&,const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
signal_contracts::Status execute_music(const PlanNode&,State&,const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
bool resampler_config(const domain::ResolvedProcessingNode&,dsp_kernels::ResamplerConfig&) noexcept;
// Copies each borrowed plane into bounded owning storage. Emits only matching
// signal intervals; would_block means one of the connected planes is pending.
signal_contracts::Status accept_fourier(const PlanNode&,State&,std::size_t,
    const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
}
