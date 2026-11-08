// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/descriptor_composition.h>
#include "composition_support.h"
#include "descriptor_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,const detail::DescriptorPlan& plan,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    auto contract=detail::composition_contract(DType::Float32,Semantic::Numeric);
    const auto shape=detail::descriptor_output_shape(plan,port);contract.shape.rank=shape.rank;contract.shape.extents=shape.extents;
    if(shape.rank!=Rank::Scalar)contract.shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)contract.shape.axes[1]=Axis::Declared;contract.unit=NumericUnit::Values;contract.clock=clock;
    return detail::composition_append_output(node,port,contract,limits,metadata);
}
}
signal_contracts::Status make_descriptor_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::descriptor_operation_kind(kind))return Status::unavailable;
    auto candidate=detail::composition_node(id,kind,std::move(parameters));
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::DescriptorPlan plan;
    const auto view=detail::composition_view(candidate);NumericContract magnitude;
    if(!detail::plan_descriptor_operation(view,plan) || !detail::composition_numeric_port(candidate,true,0,magnitude))return Status::invalid_argument;
    const auto count=detail::descriptor_output_count(plan);return detail::composition_finish(candidate,output,count,[&](domain::PortId port){return append_output(candidate,plan,port,magnitude.clock,limits,metadata);});
}
}
