// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/contrast_composition.h>
#include "contrast_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,const detail::ContrastPlan& plan,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const bool counter=plan.accumulator?port==1:port==3;
    const std::array<std::uint32_t,1> channels{0};NumericContract contract;
    contract.dtype=counter?DType::UInt64:DType::Float32;contract.channel_order=channels;
    contract.shape.rank=counter?Rank::Scalar:Rank::Vector;contract.unit=port?NumericUnit::Values:NumericUnit::Seconds;
    if(port)contract.clock=clock;
    if(!counter){contract.shape.axes[0]=Axis::Declared;contract.shape.extents[0]={0,port?plan.maximum:plan.events};}
    domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(contract,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_contrast_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::contrast_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::ContrastPlan plan;
    const auto view=detail::definition_view(candidate);NumericContract source;
    if(!detail::plan_contrast_operation(view,plan) || !detail::numeric_port(view,true,0,source))return Status::invalid_argument;
    const auto count=detail::contrast_output_count(plan);candidate.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto status=append_output(candidate,plan,domain::PortId(i),source.clock,limits,metadata);if(status!=Status::ok)return status;
    }
    output=std::move(candidate);return Status::ok;
}
}
