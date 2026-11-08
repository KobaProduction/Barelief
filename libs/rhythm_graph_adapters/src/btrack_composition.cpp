// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/btrack_composition.h>
#include "composition_support.h"
#include "btrack_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
detail::BTrackKind kind_of(std::string_view kind) noexcept {
    if(kind=="BTrackAdaptiveThreshold")return detail::BTrackKind::Threshold;
    if(kind=="BTrackCombResponse")return detail::BTrackKind::Response;
    if(kind=="BTrackHistory")return detail::BTrackKind::History;
    if(kind=="BTrackHistoryResample")return detail::BTrackKind::Resample;
    if(kind=="BTrackCombObservation")return detail::BTrackKind::Comb;
    if(kind=="BTrackBeatDecision")return detail::BTrackKind::Decision;
    return detail::BTrackKind::Onset;
}
void coordinates(NumericContract& value,detail::BTrackKind kind,domain::PortId port) noexcept {
    using detail::BTrackKind;
    if((kind==BTrackKind::History && port==1) || kind==BTrackKind::Resample || (kind==BTrackKind::Decision && port==4)){
        value.shape.axes[0]=Axis::Time;
        value.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{-261632,44100},{512,44100}};
    }
    if((kind==BTrackKind::Comb && port==0) || (kind==BTrackKind::Decision && port==7))
        value.coordinates[0]={CoordinateMode::Regular,NumericUnit::Values,{80,1},{2,1}};
    if(kind==BTrackKind::Response)value.coordinates[0]={CoordinateMode::Regular,NumericUnit::Frames,{1,1},{1,1}};
}
NumericContract output_contract(detail::BTrackKind kind,domain::PortId port,const ClockContract& clock,
    std::span<const std::uint32_t> channels) {
    const auto shape=detail::btrack_shape(kind,port);NumericContract value;
    value.dtype=shape.dtype;value.channel_order=channels;value.clock=clock;value.unit=NumericUnit::Values;
    value.shape.rank=shape.rank;value.shape.extents={ExtentBounds{shape.count,shape.count},ExtentBounds{1,1}};
    value.representation.alignment=shape.dtype==DType::Float32?alignof(float):alignof(std::int64_t);
    if(shape.rank==Rank::Vector)value.shape.axes[0]=Axis::Declared;
    coordinates(value,kind,port);return value;
}
Status append(domain::ConfiguredProcessingNode& node,detail::BTrackKind kind,domain::PortId port,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const std::array<std::uint32_t,1> channels{0};auto value=output_contract(kind,port,clock,channels);
    return detail::composition_append_output(node,port,value,limits,metadata);
}
domain::ConfiguredProcessingNode input_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters) {
    domain::ConfiguredProcessingNode result;result.id=id;result.kind=kind;result.parameters=std::move(parameters);
    result.inputs.assign(inputs.begin(),inputs.end());return result;
}
Status validate_candidate(const domain::ConfiguredProcessingNode& node) {
    detail::BTrackPlan plan;
    return detail::btrack_plan(detail::composition_view(node),plan)?Status::ok:Status::incompatible_contract;
}
Status append_outputs(domain::ConfiguredProcessingNode& node,const NumericContract& source,const FrameLimits& limits,std::size_t metadata) {
    if(node.kind=="BTrackAdaptiveThreshold")return detail::composition_append_output(node,0,source,limits,metadata);
    const auto selected=kind_of(node.kind);const auto count=detail::btrack_outputs(selected);node.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){const auto status=append(node,selected,domain::PortId(i),source.clock,limits,metadata);if(status!=Status::ok)return status;}
    return Status::ok;
}
}
signal_contracts::Status make_btrack_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::btrack_operation_kind(kind))return signal_contracts::Status::unavailable;
    auto candidate=input_node(id,kind,inputs,std::move(parameters));signal_contracts::NumericContract source;
    if(!detail::composition_numeric_port(candidate,true,0,source))return signal_contracts::Status::invalid_argument;
    const auto status=append_outputs(candidate,source,limits,metadata);if(status!=signal_contracts::Status::ok)return status;
    const auto valid=validate_candidate(candidate);if(valid!=Status::ok)return valid;
    output=std::move(candidate);return signal_contracts::Status::ok;
}
}
