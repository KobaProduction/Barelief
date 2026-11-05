// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/essentia_composition.h>
#include "essentia_operations.h"
namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_inputs(domain::ConfiguredProcessingNode& n,std::span<const domain::OwnedProcessingContract> values,const FrameLimits& limits) {
    for(std::size_t i=0;i<values.size();++i){
        if(validate_contract(values[i].view(),limits)!=Status::ok)return Status::invalid_argument;
        n.inputs.push_back({domain::PortId(i),domain::PortDirection::Input,values[i]});
    }
    return Status::ok;
}
Status append(domain::ConfiguredProcessingNode& n,domain::PortId id,const detail::BatchShape& shape,
    const FrameLimits& limits,std::size_t metadata) {
    static constexpr std::uint32_t mono=0;
    NumericContract c;c.dtype=shape.type;c.channel_order={&mono,1};c.semantic=Semantic::Numeric;c.unit=NumericUnit::Values;
    if(shape.type==DType::Float64)c.unit=NumericUnit::Seconds;
    c.representation.alignment=shape.type==DType::UInt64 || shape.type==DType::Float64?8:4;
    c.shape={shape.rank,{shape.rank==Rank::Scalar?Axis::None:Axis::Declared,shape.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{shape.variable?0:shape.rows,shape.rows},ExtentBounds{shape.columns,shape.columns}}};
    domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(c,limits,metadata,owned);if(status!=Status::ok)return status;
    n.outputs.push_back({id,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
Status append_outputs(domain::ConfiguredProcessingNode& n,const detail::EssentiaPlan& plan,const FrameLimits& limits,std::size_t metadata) {
    for(std::size_t i=0;i<plan.outputs;++i){
        const auto status=append(n,domain::PortId(i),plan.shapes[i],limits,metadata);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
signal_contracts::Status make_essentia_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::essentia_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode n;n.id=id;n.kind=kind;n.parameters=std::move(parameters);
    if(append_inputs(n,inputs,limits)!=Status::ok)return Status::invalid_argument;
    detail::EssentiaPlan plan;
    if(!detail::essentia_operation_plan(detail::definition_view(n),plan))return Status::invalid_argument;
    const auto status=append_outputs(n,plan,limits,metadata);if(status!=Status::ok)return status;
    output=std::move(n);return Status::ok;
}
}
