// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/composition.h>
#include "spectral_operation_plan.h"
#include "operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
using detail::SpectralOperationKind;
struct FrameOutputGeometry {Rank rank=Rank::Vector;std::size_t rows=0;};
std::size_t frame_outputs(const detail::SpectralOperationPlan& plan,std::array<FrameOutputGeometry,3>& geometry) noexcept {
    if(plan.kind==SpectralOperationKind::Rms){geometry[0]={Rank::Scalar,1};return 1;}
    if(plan.kind==SpectralOperationKind::Histogram){
        geometry={FrameOutputGeometry{Rank::Scalar,1},FrameOutputGeometry{Rank::Vector,plan.histogram.histogram.bins},FrameOutputGeometry{Rank::Vector,plan.bins}};
        return 3;
    }
    geometry[0]=geometry[1]={Rank::Vector,plan.bins};return plan.kind==SpectralOperationKind::Polar?1:2;
}
Status append_frame_output(domain::ConfiguredProcessingNode& node,domain::PortId id,FrameOutputGeometry shape,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    const std::array<std::uint32_t,1> channels{0};NumericContract contract;
    contract.dtype=DType::Float32;contract.channel_order=channels;contract.unit=NumericUnit::Values;
    contract.semantic=Semantic::Numeric;contract.shape.rank=shape.rank;contract.shape.extents[0]={shape.rows,shape.rows};
    if(shape.rank==Rank::Vector)contract.shape.axes[0]=Axis::Declared;
    if(!detail::index_output_contract(contract))return Status::incompatible_contract;
    contract.clock=clock;domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(contract,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({id,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
Status build_frame_outputs(domain::ConfiguredProcessingNode& node,const detail::SpectralOperationPlan& plan,
    const ClockContract& clock,const FrameLimits& limits,std::size_t metadata) {
    std::array<FrameOutputGeometry,3> geometry{};const auto count=frame_outputs(plan,geometry);node.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto status=append_frame_output(node,domain::PortId(i),geometry[i],clock,limits,metadata);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
signal_contracts::Status make_spectral_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::spectral_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    candidate.inputs.assign(inputs.begin(),inputs.end());detail::SpectralOperationPlan plan;
    const auto view=detail::definition_view(candidate);NumericContract magnitude;
    if(!detail::plan_spectral_operation(view,plan) || !detail::spectral_port(view,true,0,magnitude))return Status::invalid_argument;
    const auto status=build_frame_outputs(candidate,plan,magnitude.clock,limits,metadata);if(status!=Status::ok)return status;
    output=std::move(candidate);return Status::ok;
}
}
