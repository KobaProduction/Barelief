// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_graph_adapters/stage_ports.h>
#include <rhythm_reference_ports/onset_descriptor.h>
#include <rhythm_reference_ports/spectral_onset.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId descriptor_control_port=65500;
enum class DescriptorKind {Onset,Spectral};
struct DescriptorPlan {
    DescriptorKind kind=DescriptorKind::Onset;
    std::size_t bins=0,bands=0,intermediate=0,kernel_bytes=0;
    bool phase=false,histogram=false;
    rhythm_reference_ports::OnsetRecipe recipe=rhythm_reference_ports::OnsetRecipe::FrequencyContent;
    rhythm_reference_ports::SpectralOnsetConfig spectral;
};
bool descriptor_operation_kind(std::string_view) noexcept;
bool plan_descriptor_operation(const dataflow_runtime::NumericOperationNodeView&,DescriptorPlan&) noexcept;
bool descriptor_output_contracts(const dataflow_runtime::NumericOperationNodeView&,const DescriptorPlan&) noexcept;
std::size_t descriptor_output_count(const DescriptorPlan&) noexcept;
signal_contracts::NumericBufferShape descriptor_output_shape(const DescriptorPlan&,domain::PortId) noexcept;
domain::ProcessingSupport assess_descriptor_operation(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy descriptor_operation_policy(const dataflow_runtime::NumericOperationNodeView&);
std::unique_ptr<AdapterOperation> prepare_descriptor_operation(const dataflow_runtime::NumericOperationNode&);

class DescriptorOperation final:public AdapterOperation {
public:
    bool prepare(const dataflow_runtime::NumericOperationNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    signal_contracts::Status process_enabled(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>) noexcept;
    bool prepare_kernel();
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    DescriptorPlan plan_;
    std::variant<rhythm_reference_ports::PreparedOnsetDescriptor,rhythm_reference_ports::PreparedSpectralOnset> kernel_;
    float result_=0;
};
}
