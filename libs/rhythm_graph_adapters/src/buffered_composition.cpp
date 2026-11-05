// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/buffered_composition.h>
#include "buffered_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,const detail::BufferedPlan& plan,domain::PortId port,
    const NumericContract& input,const FrameLimits& limits,std::size_t metadata) {
    const bool scalar=plan.kind==detail::BufferedOperationKind::Flux && !port;
    const std::array<std::uint32_t,1> channels{0};NumericContract contract;
    contract.dtype=DType::Float32;contract.channel_order=channels;contract.clock=input.clock;contract.unit=NumericUnit::Values;
    contract.shape.rank=scalar?Rank::Scalar:Rank::Vector;
    if(!scalar){contract.shape.axes[0]=Axis::Declared;contract.shape.extents[0]=input.shape.extents[0];}
    domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(contract,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_buffered_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::buffered_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::BufferedPlan plan;
    const auto view=detail::definition_view(candidate);NumericContract source;
    if(!detail::plan_buffered_operation(view,plan) || !detail::numeric_port(view,true,0,source))return Status::invalid_argument;
    const auto count=plan.kind==detail::BufferedOperationKind::Flux?3u:1u;candidate.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto status=append_output(candidate,plan,domain::PortId(i),source,limits,metadata);if(status!=Status::ok)return status;
    }
    output=std::move(candidate);return Status::ok;
}
}
