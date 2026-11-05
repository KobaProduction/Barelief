// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "frame_operation_io.h"
#include "pcm_stage_buffers.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
Status validate_input(const NumericOperationInput& input,const domain::ResolvedProcessingNode& node,const FrameLimits& limits) noexcept {
    NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    if(expected.shape.rank==Rank::Vector)return validate_vector(input,std::size_t(expected.shape.extents[0].maximum),expected,limits);
    const auto status=validate_view({input.stamp,input.value},limits);
    return status==Status::ok?validate_values(input.value,ValueDomain::FiniteOnly,limits):status;
}
}
Status SequenceSourceOperation::preflight(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    if(inputs.size()!=sequence_source_input_count(plan_.kind) || !output_contracts_match(outputs) || !pcm_stage_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    const auto node=definition_view(descriptor_->node);
    for(std::size_t i=0;i<inputs.size();++i){
        if(inputs[i].port!=i)return Status::invalid_argument;
        const auto status=validate_input(inputs[i],node,limits_);if(status!=Status::ok)return status;
    }
    if(plan_.kind==SequenceSourceKind::Fusion || plan_.kind==SequenceSourceKind::Events)
        if(inputs[1].contiguous_float32.size()!=inputs[0].value.actual[0])return Status::invalid_argument;
    if(plan_.kind==SequenceSourceKind::Difference && inputs[0].value.actual[1]!=plan_.features)return Status::invalid_argument;
    for(auto& output:outputs){
        const auto shape=sequence_source_shape(plan_,output.port);output.value.actual={shape.rows,shape.columns};
        const auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
