// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/predictive_composition.h>
#include "predictive_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append(domain::ConfiguredProcessingNode& node,const detail::PredictivePlan& plan,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const std::array<std::uint32_t,1> channels{0};const auto shape=detail::predictive_shape(plan,port);NumericContract value;
    value.dtype=shape.dtype;value.channel_order=channels;value.clock=clock;value.unit=NumericUnit::Values;
    value.shape.rank=shape.rank;value.shape.extents[0]={shape.rows,shape.rows};value.shape.extents[1]={shape.columns,shape.columns};
    if(shape.rank!=Rank::Scalar)value.shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)value.shape.axes[1]=Axis::Declared;
    domain::OwnedProcessingContract owned;const auto status=domain::own_processing_contract(value,limits,metadata,owned);
    if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_predictive_operation_node(domain::NodeId id,std::string_view kind,
    const domain::OwnedProcessingContract& input,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    const std::array<domain::ProcessingPort,1> ports{{{0,domain::PortDirection::Input,input}}};
    return make_predictive_operation_node(id,kind,ports,std::move(parameters),limits,metadata,output);
}
signal_contracts::Status make_predictive_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::predictive_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::PredictivePlan plan;
    const auto node=detail::definition_view(candidate);NumericContract source;
    if(!detail::predictive_operation_plan(node,plan) || !detail::numeric_port(node,true,0,source))return Status::invalid_argument;
    const auto count=detail::predictive_output_count(plan.kind);candidate.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){const auto status=append(candidate,plan,domain::PortId(i),source.clock,limits,metadata);if(status!=Status::ok)return status;}
    output=std::move(candidate);return Status::ok;
}
}
