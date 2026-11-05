// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/pcm_stage_composition.h>
#include "pcm_stage_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
NumericContract output_contract(const detail::PcmStageOutputSpec& spec,const ClockContract& clock,std::span<const std::uint32_t> channels) noexcept {
    NumericContract contract;contract.dtype=spec.dtype;contract.channel_order=channels;contract.clock=clock;
    contract.shape.rank=spec.rank;contract.unit=NumericUnit::Values;
    contract.shape.extents[0]={spec.variable?0:spec.rows,spec.rows};contract.shape.extents[1]={spec.columns,spec.columns};
    if(spec.rank!=Rank::Scalar)contract.shape.axes[0]=Axis::Declared;
    if(spec.rank==Rank::Matrix)contract.shape.axes[1]=Axis::Declared;
    if(spec.axis!=detail::PcmStageAxis::Index)contract.coordinates[0]={CoordinateMode::ExplicitFloat64LE,
        spec.axis==detail::PcmStageAxis::Frequency?NumericUnit::Hertz:NumericUnit::Seconds};
    return contract;
}
Status append_output(domain::ConfiguredProcessingNode& node,const detail::PcmStagePlan& plan,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const std::array<std::uint32_t,1> channels{0};
    const auto contract=output_contract(detail::pcm_stage_output_spec(plan,port),clock,channels);
    domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(contract,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_pcm_stage_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::pcm_stage_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::PcmStagePlan plan;
    const auto view=detail::definition_view(candidate);NumericContract source;
    if(!detail::plan_pcm_stage(view,plan) || !detail::numeric_port(view,true,0,source))return Status::invalid_argument;
    const auto count=detail::pcm_stage_output_count(plan);candidate.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto status=append_output(candidate,plan,domain::PortId(i),source.clock,limits,metadata);if(status!=Status::ok)return status;
    }
    output=std::move(candidate);return Status::ok;
}
}
