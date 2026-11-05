// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/tracking_composition.h>
#include "tracking_operations.h"
#include "frame_operation_io.h"
namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,domain::PortId port,const detail::BatchShape& shape,const FrameLimits& limits,std::size_t metadata){
    static constexpr std::uint32_t mono=0;NumericContract c;c.dtype=shape.type;c.channel_order={&mono,1};c.semantic=Semantic::Numeric;c.unit=NumericUnit::Values;
    c.representation.alignment=shape.type==DType::Float64 || shape.type==DType::UInt64?8:shape.type==DType::UInt8?1:4;
    c.shape={shape.rank,{shape.rank==Rank::Scalar?Axis::None:Axis::Declared,shape.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{shape.variable?0:shape.rows,shape.rows},ExtentBounds{shape.columns,shape.columns}}};
    detail::TrackingPlan plan;
    if(!detail::tracking_operation_plan(detail::definition_view(node),plan))return Status::invalid_argument;
    if(plan.frame){NumericContract input;if(!detail::numeric_port(detail::definition_view(node),true,0,input))return Status::invalid_argument;c.clock=input.clock;}
    domain::OwnedProcessingContract owned;const auto status=domain::own_processing_contract(c,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_tracking_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    std::vector<domain::ProcessingPort> ports;ports.reserve(inputs.size());
    for(std::size_t i=0;i<inputs.size();++i)ports.push_back({domain::PortId(i),domain::PortDirection::Input,inputs[i]});
    return make_tracking_operation_node(id,kind,ports,std::move(parameters),limits,metadata,output);
}
signal_contracts::Status make_tracking_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    if(!detail::tracking_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    for(const auto& input:inputs)if(validate_contract(input.contract.view(),limits)!=Status::ok)return Status::invalid_argument;
    candidate.inputs.assign(inputs.begin(),inputs.end());
    detail::TrackingPlan plan;
    if(!detail::tracking_operation_plan(detail::definition_view(candidate),plan) || !detail::tracking_operation_shapes(detail::definition_view(candidate),plan))return Status::invalid_argument;
    candidate.outputs.reserve(plan.outputs);
    for(std::size_t k=0;k<plan.outputs;++k){const auto status=append_output(candidate,domain::PortId(k),plan.shape[k],limits,metadata);if(status!=Status::ok)return status;}
    output=std::move(candidate);return Status::ok;
}
}
