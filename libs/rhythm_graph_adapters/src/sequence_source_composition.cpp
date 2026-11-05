// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/sequence_source_composition.h>
#include "sequence_source_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append(domain::ConfiguredProcessingNode& node,const detail::SequenceSourcePlan& plan,domain::PortId port,
    const FrameLimits& limits,std::size_t metadata) {
    const auto shape=detail::sequence_source_shape(plan,port);const std::array<std::uint32_t,1> channels{0};NumericContract value;
    value.dtype=shape.dtype;value.channel_order=channels;value.unit=NumericUnit::Values;value.shape.rank=shape.rank;
    value.shape.extents[0]={shape.variable?0:shape.rows,shape.rows};value.shape.extents[1]={shape.columns,shape.columns};
    if(shape.rank!=Rank::Scalar)value.shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)value.shape.axes[1]=Axis::Declared;
    domain::OwnedProcessingContract owned;const auto status=domain::own_processing_contract(value,limits,metadata,owned);
    if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_sequence_source_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::sequence_source_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);candidate.inputs.reserve(inputs.size());
    for(std::size_t i=0;i<inputs.size();++i)candidate.inputs.push_back({domain::PortId(i),domain::PortDirection::Input,inputs[i]});
    detail::SequenceSourcePlan plan;if(!detail::sequence_source_plan(detail::definition_view(candidate),plan))return Status::invalid_argument;
    const auto count=detail::sequence_source_output_count(plan.kind);candidate.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){const auto status=append(candidate,plan,domain::PortId(i),limits,metadata);if(status!=Status::ok)return status;}
    output=std::move(candidate);return Status::ok;
}
}
