// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/buffered_composition.h>
#include "composition_support.h"
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
    return detail::composition_append_output(node,port,contract,limits,metadata);
}
}
signal_contracts::Status make_buffered_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::buffered_operation_kind(kind))return Status::unavailable;
    auto candidate=detail::composition_node(id,kind,std::move(parameters));
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::BufferedPlan plan;
    const auto view=detail::composition_view(candidate);NumericContract source;
    if(!detail::plan_buffered_operation(view,plan) || !detail::composition_numeric_port(candidate,true,0,source))return Status::invalid_argument;
    const auto count=plan.kind==detail::BufferedOperationKind::Flux?3u:1u;return detail::composition_finish(candidate,output,count,[&](domain::PortId port){return append_output(candidate,plan,port,source,limits,metadata);});
}
}
