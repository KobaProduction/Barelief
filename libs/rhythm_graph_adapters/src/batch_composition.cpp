// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/batch_composition.h>
#include "composition_support.h"
#include "batch_operation_plan.h"
#include "frame_operation_io.h"
namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,domain::PortId port,const detail::BatchShape& shape,const FrameLimits& limits,std::size_t metadata){
    auto c=detail::composition_contract(shape.type,Semantic::Numeric);
    c.shape={shape.rank,{shape.rank==Rank::Scalar?Axis::None:Axis::Declared,shape.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{shape.variable?0:shape.rows,shape.rows},ExtentBounds{shape.columns,shape.columns}}};
    return detail::composition_append_output(node,port,c,limits,metadata);
}
}
signal_contracts::Status make_batch_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output){
    if(!detail::batch_operation_kind(kind))return Status::unavailable;
    auto candidate=detail::composition_node(id,kind,std::move(parameters));
    const auto input_status=detail::composition_inputs(candidate,inputs,limits);if(input_status!=Status::ok)return input_status;
    detail::BatchPlan plan;
    if(!detail::batch_operation_plan(detail::composition_view(candidate),plan) || inputs.size()!=detail::batch_operation_input_count(plan.kind) || !detail::batch_operation_shapes(detail::composition_view(candidate),plan))return Status::invalid_argument;
    return detail::composition_finish(candidate,output,plan.outputs,[&](domain::PortId port){return append_output(candidate,port,plan.shape[std::size_t(port)],limits,metadata);});
}
}
