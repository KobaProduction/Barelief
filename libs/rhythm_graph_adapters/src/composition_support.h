// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation_binding.h>
#include <utility>

namespace rhythm_graph_adapters::detail {
// Only graph composition can see full domain contracts. Numerical plans receive
// the restricted projection, with no conversion back to the domain node.
inline dataflow_runtime::NumericOperationNodeView composition_view(const domain::ConfiguredProcessingNode& node) noexcept {
    domain::ResolvedProcessingNode full;full.id=node.id;full.kind=node.kind;full.parameters=&node.parameters;
    full.inputs=node.inputs;full.outputs=node.outputs;full.enabled=node.enabled;
    return dataflow_runtime::numeric_operation_node_view(full);
}
inline bool composition_numeric_port(const domain::ConfiguredProcessingNode& node,bool input,domain::PortId id,
    signal_contracts::NumericContract& result) noexcept {
    for(const auto& port:input?node.inputs:node.outputs){
        if(port.id!=id)continue;
        const auto value=port.contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&value);
        if(!numeric)return false;
        result=*numeric;return true;
    }
    return false;
}
inline bool composition_index_coordinates(const signal_contracts::AxisCoordinates& axis) noexcept {
    using namespace signal_contracts;
    if(axis.mode==CoordinateMode::None)return axis==AxisCoordinates{};
    return axis.mode==CoordinateMode::Regular && (axis.unit==NumericUnit::Values || axis.unit==NumericUnit::Frames) &&
        axis.origin.numerator==0 && axis.origin.denominator && axis.step.numerator>0 &&
        std::uint64_t(axis.step.numerator)==axis.step.denominator;
}
inline bool composition_index_output(const signal_contracts::NumericContract& value) noexcept {
    return value.clock.domain.empty() && !value.clock.sample_rate && value.clock.tick_seconds==signal_contracts::Ratio{1,1} &&
        composition_index_coordinates(value.coordinates[0]) && composition_index_coordinates(value.coordinates[1]);
}
inline domain::ConfiguredProcessingNode composition_node(domain::NodeId id,std::string_view kind,domain::TransformProcessingParameters parameters) {
    domain::ConfiguredProcessingNode node;node.id=id;node.kind=kind;node.parameters=std::move(parameters);return node;
}
inline signal_contracts::NumericContract composition_contract(signal_contracts::DType dtype,signal_contracts::Semantic semantic) noexcept {
    static constexpr std::uint32_t mono=0;signal_contracts::NumericContract contract;
    contract.dtype=dtype;contract.channel_order={&mono,1};contract.semantic=semantic;contract.unit=signal_contracts::NumericUnit::Values;
    return contract;
}
inline signal_contracts::Status composition_append_output(domain::ConfiguredProcessingNode& node,domain::PortId port,
    const signal_contracts::NumericContract& contract,const signal_contracts::FrameLimits& limits,std::size_t metadata) {
    domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(contract,limits,metadata,owned);if(status!=signal_contracts::Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return signal_contracts::Status::ok;
}
inline signal_contracts::Status composition_inputs(domain::ConfiguredProcessingNode& node,
    std::span<const domain::OwnedProcessingContract> inputs,const signal_contracts::FrameLimits& limits) {
    node.inputs.reserve(inputs.size());
    for(std::size_t i=0;i<inputs.size();++i){
        if(signal_contracts::validate_contract(inputs[i].view(),limits)!=signal_contracts::Status::ok)return signal_contracts::Status::invalid_argument;
        node.inputs.push_back({domain::PortId(i),domain::PortDirection::Input,inputs[i]});
    }
    return signal_contracts::Status::ok;
}
inline bool composition_retain_clock(const domain::ConfiguredProcessingNode& node,signal_contracts::NumericContract& output) noexcept {
    signal_contracts::NumericContract input;if(!composition_numeric_port(node,true,0,input))return false;
    output.clock=input.clock;return true;
}
inline signal_contracts::Status composition_inputs(domain::ConfiguredProcessingNode& node,
    std::span<const domain::ProcessingPort> inputs,const signal_contracts::FrameLimits& limits) {
    for(const auto& input:inputs)if(signal_contracts::validate_contract(input.contract.view(),limits)!=signal_contracts::Status::ok)return signal_contracts::Status::invalid_argument;
    node.inputs.assign(inputs.begin(),inputs.end());return signal_contracts::Status::ok;
}
template<class Append> signal_contracts::Status composition_finish(domain::ConfiguredProcessingNode& node,
    domain::ConfiguredProcessingNode& output,std::size_t count,Append append) {
    node.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){const auto status=append(domain::PortId(i));if(status!=signal_contracts::Status::ok)return status;}
    output=std::move(node);return signal_contracts::Status::ok;
}

}
