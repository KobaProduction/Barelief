// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/sequence_source_composition.h>
#include "composition_support.h"
#include "sequence_source_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append(domain::ConfiguredProcessingNode& node,const detail::SequenceSourcePlan& plan,domain::PortId port,
    const FrameLimits& limits,std::size_t metadata) {
    const auto shape=detail::sequence_source_shape(plan,port);auto value=detail::composition_contract(shape.dtype,Semantic::Scalar);value.shape.rank=shape.rank;
    value.shape.extents[0]={shape.variable?0:shape.rows,shape.rows};value.shape.extents[1]={shape.columns,shape.columns};
    if(shape.rank!=Rank::Scalar)value.shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)value.shape.axes[1]=Axis::Declared;
    return detail::composition_append_output(node,port,value,limits,metadata);
}
}
signal_contracts::Status make_sequence_source_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::sequence_source_kind(kind))return Status::unavailable;
    auto candidate=detail::composition_node(id,kind,std::move(parameters));candidate.inputs.reserve(inputs.size());
    for(std::size_t i=0;i<inputs.size();++i)candidate.inputs.push_back({domain::PortId(i),domain::PortDirection::Input,inputs[i]});
    detail::SequenceSourcePlan plan;if(!detail::sequence_source_plan(detail::composition_view(candidate),plan))return Status::invalid_argument;
    const auto count=detail::sequence_source_output_count(plan.kind);return detail::composition_finish(candidate,output,count,[&](domain::PortId port){return append(candidate,plan,port,limits,metadata);});
}
}
