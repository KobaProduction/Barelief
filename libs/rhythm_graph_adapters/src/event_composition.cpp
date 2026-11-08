// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/event_composition.h>
#include "composition_support.h"
#include "event_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,const detail::EventPlan& plan,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const std::array<std::uint32_t,1> channels{0};NumericContract contract;
    contract.dtype=detail::event_output_dtype(plan,port);contract.channel_order=channels;
    contract.shape.rank=Rank::Scalar;contract.unit=NumericUnit::Values;contract.clock=clock;
    return detail::composition_append_output(node,port,contract,limits,metadata);
}
}
signal_contracts::Status make_event_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::event_operation_kind(kind))return Status::unavailable;
    auto candidate=detail::composition_node(id,kind,std::move(parameters));
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::EventPlan plan;
    const auto view=detail::composition_view(candidate);NumericContract source;
    if(!detail::plan_event_operation(view,plan) || !detail::composition_numeric_port(candidate,true,0,source))return Status::invalid_argument;
    const auto count=detail::event_output_count(plan);return detail::composition_finish(candidate,output,count,[&](domain::PortId port){return append_output(candidate,plan,port,source.clock,limits,metadata);});
}
}
