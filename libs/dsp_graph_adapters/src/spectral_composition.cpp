#include <dsp_graph_adapters/spectral_composition.h>
#include "spectral_sequence.h"
#include <array>

namespace dsp_graph_adapters {
namespace {
using namespace signal_contracts;
Status append_output(domain::ConfiguredProcessingNode& node,const detail::SpectralSequencePlan& plan,domain::PortId port,
    const FrameLimits& limits,std::size_t metadata) {
    static constexpr std::uint32_t mono=0;const auto shape=detail::spectral_output_shape(plan,port);
    NumericContract c;c.dtype=shape.dtype;c.channel_order={&mono,1};c.shape.rank=shape.rank;
    c.shape.extents={ExtentBounds{shape.rank==Rank::Scalar?1u:0u,shape.rows},ExtentBounds{shape.columns,shape.columns}};
    if(shape.rank!=Rank::Scalar)c.shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)c.shape.axes[1]=Axis::Declared;
    c.representation.alignment=shape.dtype==DType::UInt64 || shape.dtype==DType::Int64?8:4;
    domain::OwnedProcessingContract owned;const auto status=domain::own_processing_contract(c,limits,metadata,owned);
    if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
}
signal_contracts::Status make_spectral_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& out) {
    const auto* spec=domain::sequence_operation_spec(kind);
    if(!spec || spec->family!=domain::SequenceOperationFamily::Spectral)return Status::unavailable;
    domain::ConfiguredProcessingNode node;node.id=id;node.kind=kind;node.inputs.assign(inputs.begin(),inputs.end());node.parameters=std::move(parameters);
    detail::SpectralSequencePlan plan;if(!detail::spectral_sequence_plan(detail::definition(node),plan))return Status::invalid_argument;
    node.outputs.reserve(spec->outputs);
    for(std::size_t i=0;i<spec->outputs;++i){const auto status=append_output(node,plan,domain::PortId(i),limits,metadata);if(status!=Status::ok)return status;}
    out=std::move(node);return Status::ok;
}
}
