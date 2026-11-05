// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/buffered_band_flux.h>
#include <rhythm_reference_ports/causal_average.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId buffered_control_port=65500;
enum class BufferedOperationKind {Maximum,Average,Flux};
struct BufferedPlan {
    BufferedOperationKind kind=BufferedOperationKind::Maximum;
    std::size_t maximum=0,width=3,kernel_bytes=0,buffer_bytes=0,maximum_frames=3;
    bool causal=true;
    rhythm_reference_ports::BufferedBandFluxConfig flux;
};
bool buffered_operation_kind(std::string_view) noexcept;
bool plan_buffered_operation(const domain::ResolvedProcessingNode&,BufferedPlan&) noexcept;
bool buffered_output_contracts(const domain::ResolvedProcessingNode&,const BufferedPlan&) noexcept;
domain::ProcessingSupport assess_buffered_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy buffered_operation_policy(const domain::ResolvedProcessingNode&);
std::unique_ptr<AdapterOperation> prepare_buffered_operation(const domain::ConfiguredProcessingNode&);

class BufferedOperation final:public AdapterOperation {
public:
    bool prepare(const domain::ConfiguredProcessingNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&,std::size_t) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&,std::size_t) noexcept;
    BufferedPlan plan_;
    std::variant<rhythm_reference_ports::PreparedBufferedMaximum,rhythm_reference_ports::PreparedCausalAverage,
        rhythm_reference_ports::PreparedBufferedBandFlux> kernel_;
    std::vector<float> values_;
    float result_=0;
};
}
