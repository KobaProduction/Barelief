// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/composition.h>
#include "adapter_operation.h"
#include "operation_io.h"
#include "parameter_values.h"
#include <algorithm>

namespace rhythm_graph_adapters {
namespace {
using namespace signal_contracts;
struct OutputGeometry {Rank rank=Rank::Vector;std::size_t rows=0,columns=1;bool variable=false;NumericUnit unit=NumericUnit::Values;};
bool output_geometry(const domain::ConfiguredProcessingNode& node,std::size_t maximum,
    std::array<OutputGeometry,4>& geometry,std::size_t& count) noexcept {
    count=1;
    if(node.kind=="HarmonicTempoCandidates"){geometry[0]={Rank::Vector,maximum,1,true};return true;}
    if(node.kind=="TempoChangeSections"){
        count=2;geometry[0]=geometry[1]={Rank::Vector,maximum,1,true,NumericUnit::Seconds};return true;
    }
    if(node.kind=="BeatIntervalHistogram"){
        count=2;geometry[0]={Rank::Vector,250};geometry[1]={Rank::Matrix,2,3};return true;
    }
    const auto scales=detail::array_parameter(node,"scales");if(scales.empty())return false;
    count=4;geometry={OutputGeometry{Rank::Scalar,1},OutputGeometry{Rank::Vector,scales.size()},
        OutputGeometry{Rank::Vector,scales.size()-1},OutputGeometry{Rank::Scalar,1}};return true;
}
Status append_output(domain::ConfiguredProcessingNode& node,domain::PortId port,OutputGeometry geometry,
    const FrameLimits& limits,std::size_t metadata) {
    const std::array<std::uint32_t,1> channels{0};NumericContract contract;
    contract.dtype=DType::Float32;contract.channel_order=channels;contract.unit=geometry.unit;
    contract.semantic=Semantic::Numeric;contract.shape.rank=geometry.rank;
    contract.shape.extents={ExtentBounds{geometry.variable?0:geometry.rows,std::max(std::size_t{1},geometry.rows)},ExtentBounds{geometry.columns,geometry.columns}};
    if(geometry.rank!=Rank::Scalar)contract.shape.axes={Axis::Declared,geometry.rank==Rank::Matrix?Axis::Component:Axis::None};
    if(!detail::index_output_contract(contract))return Status::incompatible_contract;
    domain::OwnedProcessingContract owned;const auto status=domain::own_processing_contract(contract,limits,metadata,owned);
    if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_interval_operation_node(domain::NodeId id,std::string_view kind,
    const domain::OwnedProcessingContract& input,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& output) {
    if(!detail::interval_operation_kind(kind))return Status::unavailable;
    domain::ConfiguredProcessingNode candidate;candidate.id=id;candidate.kind=kind;candidate.parameters=std::move(parameters);
    candidate.inputs.push_back({0,domain::PortDirection::Input,input});std::size_t maximum=0,count=0;
    std::array<OutputGeometry,4> geometry{};
    if(!detail::vector_capacity(candidate,domain::PortDirection::Input,0,maximum) || !maximum ||
       !output_geometry(candidate,maximum,geometry,count))return Status::invalid_argument;
    candidate.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){const auto status=append_output(candidate,domain::PortId(i),geometry[i],limits,metadata);if(status!=Status::ok)return status;}
    output=std::move(candidate);return Status::ok;
}
}
