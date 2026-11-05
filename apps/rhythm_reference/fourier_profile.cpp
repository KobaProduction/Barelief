// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fourier_profile.h"
#include <kiss_backend.h>
#include <ooura_backend.h>
#include <stdexcept>

namespace {
std::string_view requested_backend(const domain::ConfiguredProcessingNode& node) {
    if(node.kind!="Fourier" && node.kind!="RealFourier")return {};
    const domain::ResolvedProcessingNode resolved{node.id,node.kind,domain::NodeRole::Processor,domain::ProcessingOperation::Fourier,
        node.inputs,node.outputs,&node.parameters,node.enabled};
    domain::ProcessingParameterView value;
    if(domain::resolve_processing_parameter(resolved,"backend_id",value)!=signal_contracts::Status::ok)
        throw std::invalid_argument("Invalid Fourier backend_id");
    const auto* id=std::get_if<std::string_view>(&value);
    if(!id)throw std::invalid_argument("Fourier backend_id must be a string");
    return *id;
}
const dsp_kernels::FourierBackendFactory* named_backend(std::string_view id) {
    for(const auto* factory:{&desktop_fourier::kiss_backend_factory(),&desktop_fourier::ooura_backend_factory()})
        if(id==factory->id())return factory;
    throw std::invalid_argument("Unknown research Fourier backend_id");
}
}
std::shared_ptr<const dsp_kernels::FourierBackendFactory> research_fourier_backend(const domain::NumericProcessingGraph& graph) {
    const dsp_kernels::FourierBackendFactory* selected=nullptr;
    for(const auto& node:graph.nodes) {
        const auto id=requested_backend(node);if(id.empty())continue;
        const auto* factory=named_backend(id);
        if(selected && selected!=factory)throw std::invalid_argument("Conflicting research Fourier backend_id values");
        selected=factory;
    }
    if(!selected)return {};
    return {selected,[](const auto*){}}; // Installed factories have static lifetime.
}
