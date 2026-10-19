#pragma once
#include <domain/graph/admission.h>
namespace domain {
struct NodeFactorySettings {
    std::uint32_t sample_rate=48000;
    std::uint32_t fft_size=4096;
    std::uint32_t packet_samples=256;
    std::uint32_t history_capacity=400;
};
// Creates an editable node with registered parameters and reusable contract derivation.
// No hardware probing, JSON, UI defaults or graph ancestry participates.
ProcessingDiagnostic make_processing_node(std::string_view kind,NodeId,
    const ProcessingAdmissionLimits&,ConfiguredProcessingNode&,NodeFactorySettings={});
ProcessingDiagnostic make_processing_node(NodeKind,NodeId,
    const ProcessingAdmissionLimits&,ConfiguredProcessingNode&,NodeFactorySettings={});
ProcessingDiagnostic derive_processing_node_contracts(ConfiguredProcessingNode&,
    const ProcessingAdmissionLimits&);
// Assign a declared primary input and derive output contracts atomically.
// PcmResampler and Window materialize this input configuration as input_rate;
// their scalar Fs bindings can override it. A zero input_rate means unavailable,
// while a nonzero rate inconsistent with the declared input is incompatible.
ProcessingDiagnostic configure_processing_input(ConfiguredProcessingNode&,const signal_contracts::DataContract&,
    const ProcessingAdmissionLimits&);
}
