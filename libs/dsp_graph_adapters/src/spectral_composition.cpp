#include <dsp_graph_adapters/spectral_composition.h>
#include "spectral_sequence.h"
#include <dataflow_runtime/numeric_operation_binding.h>
#include <array>

namespace dsp_graph_adapters {
namespace {
using namespace signal_contracts;
domain::ConfiguredProcessingNode spectral_input_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters) {
    domain::ConfiguredProcessingNode node;node.id=id;node.kind=kind;
    node.inputs.assign(inputs.begin(),inputs.end());node.parameters=std::move(parameters);return node;
}
NumericContract spectral_contract(const detail::SpectralOutputShape& shape) {
    static constexpr std::uint32_t mono=0;
    NumericContract contract;contract.dtype=shape.dtype;contract.channel_order={&mono,1};contract.shape.rank=shape.rank;
    contract.shape.extents={ExtentBounds{shape.rank==Rank::Scalar?1u:0u,shape.rows},ExtentBounds{shape.columns,shape.columns}};
    if(shape.rank!=Rank::Scalar)contract.shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)contract.shape.axes[1]=Axis::Declared;
    contract.representation.alignment=shape.dtype==DType::UInt64 || shape.dtype==DType::Int64?8:4;
    return contract;
}
Status append_output(domain::ConfiguredProcessingNode& node,const detail::SpectralSequencePlan& plan,domain::PortId port,
    const FrameLimits& limits,std::size_t metadata) {
    const auto contract=spectral_contract(detail::spectral_output_shape(plan,port));domain::OwnedProcessingContract owned;
    const auto status=domain::own_processing_contract(contract,limits,metadata,owned);if(status!=Status::ok)return status;
    node.outputs.push_back({port,domain::PortDirection::Output,std::move(owned)});return Status::ok;
}
Status append_outputs(domain::ConfiguredProcessingNode& node,const detail::SpectralSequencePlan& plan,std::size_t count,
    const FrameLimits& limits,std::size_t metadata) {
    node.outputs.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto status=append_output(node,plan,domain::PortId(i),limits,metadata);if(status!=Status::ok)return status;
    }
    return Status::ok;
}

}
signal_contracts::Status make_spectral_operation_node(domain::NodeId id,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits& limits,std::size_t metadata,domain::ConfiguredProcessingNode& out) {
    const auto* spec=domain::sequence_operation_spec(kind);
    if(!spec || spec->family!=domain::SequenceOperationFamily::Spectral)return Status::unavailable;
    auto node=spectral_input_node(id,kind,inputs,std::move(parameters));
    const auto operation=dataflow_runtime::numeric_operation_node(node);
    detail::SpectralSequencePlan plan;if(!detail::spectral_sequence_plan(detail::definition(operation),plan))return Status::invalid_argument;
    const auto status=append_outputs(node,plan,spec->outputs,limits,metadata);if(status!=Status::ok)return status;
    out=std::move(node);return Status::ok;
}
}
