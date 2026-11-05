// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/loop_composition.h>
#include <rhythm_graph_adapters/loudness_composition.h>
#include "loudness_operations.h"
namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status output_port(domain::ConfiguredProcessingNode& node,domain::PortId id,const detail::BatchShape& shape,const FrameLimits& limits,std::size_t metadata,bool variable_columns){
    static constexpr std::uint32_t mono=0;NumericContract c;c.dtype=shape.type;c.channel_order={&mono,1};c.semantic=Semantic::Numeric;c.unit=NumericUnit::Values;
    c.shape={shape.rank,{shape.rank==Rank::Scalar?Axis::None:Axis::Declared,shape.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{shape.variable?0:shape.rows,shape.rows},ExtentBounds{variable_columns?0:shape.columns,shape.columns}}};
    domain::OwnedProcessingContract owned;const auto status=domain::own_processing_contract(c,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({id,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
Status make(domain::NodeId id,std::string_view kind,std::span<const domain::OwnedProcessingContract> in,domain::TransformProcessingParameters params,
    const FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    if(!detail::loudness_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode next;next.id=id;next.kind=kind;next.parameters=std::move(params);next.inputs.reserve(in.size());
    for(std::size_t k=0;k<in.size();++k){if(validate_contract(in[k].view(),limits)!=Status::ok)return Status::invalid_argument;next.inputs.push_back({domain::PortId(k),domain::PortDirection::Input,in[k]});}
    detail::LoudnessPlan plan;if(!detail::loudness_operation_plan(detail::definition_view(next),plan))return Status::invalid_argument;
    next.outputs.reserve(plan.outputs);
    for(std::size_t k=0;k<plan.outputs;++k){const auto status=output_port(next,domain::PortId(k),plan.shape[k],limits,metadata,plan.kind==detail::LoudnessKind::Salience && k==0);if(status!=Status::ok)return status;}
    output=std::move(next);return Status::ok;
}
}
Status make_loop_operation_node(domain::NodeId id,std::string_view kind,std::span<const domain::OwnedProcessingContract> in,domain::TransformProcessingParameters params,
    const FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    if(kind!="LoopLengthFit" && kind!="LoopPcmTempo")return Status::unavailable;
    return make(id,kind,in,std::move(params),limits,metadata,output);
}
Status make_loudness_operation_node(domain::NodeId id,std::string_view kind,std::span<const domain::OwnedProcessingContract> in,domain::TransformProcessingParameters params,
    const FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    if(kind=="LoopLengthFit" || kind=="LoopPcmTempo")return Status::unavailable;
    return make(id,kind,in,std::move(params),limits,metadata,output);
}
}
