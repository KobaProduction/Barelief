// SPDX-License-Identifier: AGPL-3.0-or-later
#include "event_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool EventOperation::prepare(const domain::ConfiguredProcessingNode& node) {
    const auto view=definition_view(node);
    if(!plan_event_operation(view,plan_) || !event_output_contracts(view,plan_))return false;
    bool ready=true;
    if(plan_.kind==EventOperationKind::Picker)ready=kernel_.emplace<PreparedAdaptivePeakPicker>().prepare(plan_.picker);
    else if(plan_.kind==EventOperationKind::Decision)ready=kernel_.emplace<PreparedOnsetDecision>().prepare(plan_.decision);
    else kernel_.emplace<std::monostate>();
    return ready && workspace_bytes()<=plan_.kernel_bytes;
}
std::unique_ptr<AdapterOperation> prepare_event_operation(const domain::ConfiguredProcessingNode& node) {
    if(!event_operation_kind(node.kind))return {};
    auto operation=std::make_unique<EventOperation>();if(!operation->prepare(node))return {};
    return operation;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> EventOperation::clone() const {return std::make_unique<EventOperation>(*this);}
std::size_t EventOperation::workspace_bytes() const noexcept {
    return plan_.kind==EventOperationKind::Picker?std::get<PreparedAdaptivePeakPicker>(kernel_).workspace_bytes():0;
}
void EventOperation::reset() noexcept {
    if(plan_.kind==EventOperationKind::Picker)std::get<PreparedAdaptivePeakPicker>(kernel_).reset();
    else if(plan_.kind==EventOperationKind::Decision)std::get<PreparedOnsetDecision>(kernel_).reset();
    peak_={};decision_={};silent_=false;
}
signal_contracts::Status EventOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation& source) noexcept {
    const auto* other=dynamic_cast<const EventOperation*>(&source);
    if(!other || !compatible(*other) || plan_.kind==EventOperationKind::Picker){reset();return signal_contracts::Status::incompatible_contract;}
    if(plan_.kind==EventOperationKind::Decision)std::get<PreparedOnsetDecision>(kernel_)=std::get<PreparedOnsetDecision>(other->kernel_);
    decision_=other->decision_;silent_=other->silent_;return signal_contracts::Status::ok;
}
}
