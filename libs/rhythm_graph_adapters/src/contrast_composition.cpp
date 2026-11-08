// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/contrast_composition.h>
#include "composition_support.h"
#include "contrast_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,const detail::ContrastPlan& plan,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const bool counter=plan.accumulator?port==1:port==3;
    auto contract=detail::composition_contract(counter?DType::UInt64:DType::Float32,Semantic::Scalar);
    contract.shape.rank=counter?Rank::Scalar:Rank::Vector;contract.unit=port?NumericUnit::Values:NumericUnit::Seconds;
    if(port)contract.clock=clock;
    if(!counter){contract.shape.axes[0]=Axis::Declared;contract.shape.extents[0]={0,port?plan.maximum:plan.events};}
    return detail::composition_append_output(node,port,contract,limits,metadata);
}
}
signal_contracts::Status make_contrast_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::contrast_operation_kind(kind))return Status::unavailable;
    auto candidate=detail::composition_node(id,kind,std::move(parameters));
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::ContrastPlan plan;
    const auto view=detail::composition_view(candidate);NumericContract source;
    if(!detail::plan_contrast_operation(view,plan) || !detail::composition_numeric_port(candidate,true,0,source))return Status::invalid_argument;
    const auto count=detail::contrast_output_count(plan);return detail::composition_finish(candidate,output,count,[&](domain::PortId port){return append_output(candidate,plan,port,source.clock,limits,metadata);});
}
}
