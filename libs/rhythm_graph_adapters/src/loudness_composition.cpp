// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/loop_composition.h>
#include "composition_support.h"
#include <rhythm_graph_adapters/loudness_composition.h>
#include "loudness_operations.h"
namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status output_port(domain::ConfiguredProcessingNode& node,domain::PortId id,const detail::BatchShape& shape,const FrameLimits& limits,std::size_t metadata,bool variable_columns){
    auto c=detail::composition_contract(shape.type,Semantic::Numeric);
    c.shape={shape.rank,{shape.rank==Rank::Scalar?Axis::None:Axis::Declared,shape.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{shape.variable?0:shape.rows,shape.rows},ExtentBounds{variable_columns?0:shape.columns,shape.columns}}};
    return detail::composition_append_output(node,id,c,limits,metadata);
}
Status make(domain::NodeId id,std::string_view kind,std::span<const domain::OwnedProcessingContract> in,domain::TransformProcessingParameters params,
    const FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    if(!detail::loudness_operation_kind(kind))return Status::unavailable;
    auto next=detail::composition_node(id,kind,std::move(params));const auto input_status=detail::composition_inputs(next,in,limits);if(input_status!=Status::ok)return input_status;
    detail::LoudnessPlan plan;if(!detail::loudness_operation_plan(detail::composition_view(next),plan))return Status::invalid_argument;
    return detail::composition_finish(next,output,plan.outputs,[&](domain::PortId port){return output_port(next,port,plan.shape[std::size_t(port)],limits,metadata,plan.kind==detail::LoudnessKind::Salience && std::size_t(port)==0);});
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
