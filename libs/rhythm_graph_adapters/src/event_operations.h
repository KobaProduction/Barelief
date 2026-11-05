// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <rhythm_reference_ports/onset_decision.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId event_control_port=65500;
enum class EventOperationKind {Picker,Decision,Silence};
struct EventPlan {
    EventOperationKind kind=EventOperationKind::Picker;
    std::size_t maximum=0,kernel_bytes=0;
    float silence_db=-70;
    rhythm_reference_ports::AdaptivePeakConfig picker;
    rhythm_reference_ports::OnsetDecisionConfig decision;
};
bool event_operation_kind(std::string_view) noexcept;
bool plan_event_operation(const domain::ResolvedProcessingNode&,EventPlan&) noexcept;
std::size_t event_output_count(const EventPlan&) noexcept;
signal_contracts::DType event_output_dtype(const EventPlan&,domain::PortId) noexcept;
bool event_output_contracts(const domain::ResolvedProcessingNode&,const EventPlan&) noexcept;
domain::ProcessingSupport assess_event_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy event_operation_policy(const domain::ResolvedProcessingNode&);
std::unique_ptr<AdapterOperation> prepare_event_operation(const domain::ConfiguredProcessingNode&);

class EventOperation final:public AdapterOperation {
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
    signal_contracts::Status process_enabled(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>) noexcept;
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,float& peak,bool& silent) const noexcept;
    signal_contracts::Status validate_event_outputs(std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>,float peak,bool silent) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    EventPlan plan_;
    std::variant<rhythm_reference_ports::PreparedAdaptivePeakPicker,rhythm_reference_ports::PreparedOnsetDecision,std::monostate> kernel_;
    rhythm_reference_ports::AdaptivePeakObservation peak_;
    rhythm_reference_ports::OnsetDecision decision_;
    bool silent_=false;
};
}
