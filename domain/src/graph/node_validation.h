#pragma once
#include <domain/graph/admission.h>
namespace domain::detail {
struct NodeValidationView {
    NodeId id;std::string_view kind;const ProcessingParameters& parameters;
    std::span<const ProcessingPort> inputs,outputs;bool enabled;NodeId clock_node;
};
struct OperationValidation {
    const ConfiguredProcessingNode& configured;
    const ProcessingAdmissionLimits& limits;
    const NodeValidationView& node;
    const std::optional<NodeKind>& kind;
    NodeRole role;
    ProcessingOperation operation;
    std::size_t input_count,output_count;
    const signal_contracts::DataContract& incoming;
    const signal_contracts::DataContract& outgoing;
    const signal_contracts::NumericContract* input;
    const signal_contracts::NumericContract* output;
    ProcessingDiagnostic error(ProcessingError reason,PortId port=0) const {return {reason,configured.id,port};}
    ResolvedProcessingNode resolved() const {return {node.id,node.kind,role,operation,node.inputs,node.outputs,&node.parameters,node.enabled};}
};
ProcessingDiagnostic validate_operation(const OperationValidation&) noexcept;
}
