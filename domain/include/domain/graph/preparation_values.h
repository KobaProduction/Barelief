#pragma once
#include <domain/graph/admission.h>

namespace domain {
enum class PreparationValueKind { Streaming, UnsignedInteger, FiniteScalar, Routed };
struct PreparationValueCapability {
    PreparationValueKind kind=PreparationValueKind::Streaming;
    explicit operator bool() const noexcept { return kind!=PreparationValueKind::Streaming; }
};
// This capability is distinct from scalar dtype: streaming producers cannot size buffers.
PreparationValueCapability preparation_value_capability(const ConfiguredProcessingNode&,PortId) noexcept;
struct PreparationValueProvider {
    using Resolve=signal_contracts::Status (*)(const void*,const NumericProcessingGraph&,NodeId,PortId,signal_contracts::NumericLiteral&) noexcept;
    const void* context=nullptr;
    Resolve resolve=nullptr;
};
signal_contracts::Status resolve_preparation_value(const NumericProcessingGraph&,NodeId,PortId,
    signal_contracts::NumericLiteral&,const PreparationValueProvider* provider=nullptr) noexcept;
// Ordinary scalar parameter wires are evaluated before buffers are prepared.
// The evaluated graph retains those wires; executable_graph strips them.
// changed_node seeds downstream contract derivation after a local parameter edit.
ProcessingDiagnostic resolve_numeric_parameter_bindings(const NumericProcessingGraph&,
    const ProcessingAdmissionLimits&,NumericProcessingGraph& evaluated,NumericProcessingGraph& executable_graph,NodeId changed_node=-1,const PreparationValueProvider* provider=nullptr);
}
