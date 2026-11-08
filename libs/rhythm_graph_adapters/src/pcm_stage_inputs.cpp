// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_buffers.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status scalar_source(const NumericOperationInput& input,const NumericBufferContract& expected,const FrameLimits& limits) noexcept {
    if(input.status!=Status::ok)return Status::unavailable;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(input.value,limits);if(status!=Status::ok)return status;
    std::uint8_t value=0;const auto loaded=load_element(input.value,{0,0,0},value);
    return loaded==Status::ok && value>1?Status::invalid_argument:loaded;
}
Status source_pair(std::span<const NumericOperationInput> inputs) noexcept {
    const auto* reference=input_at(inputs,0);if(!reference)return Status::unavailable;
    for(const auto& input:inputs){
        if(input.port>context_sequence_ports::autocorrelation_input)continue;
        if(input.status!=Status::ok)return Status::unavailable;
        if(!equal_frame_identity(*reference,input))return Status::incompatible_contract;
    }
    return Status::ok;
}
Status bound_inputs(std::span<const NumericOperationInput> inputs,const dataflow_runtime::NumericOperationNodeView& node) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        NumericBufferContract expected;if(!numeric_port(node,true,inputs[i].port,expected))return Status::incompatible_contract;
        if(!operation_input_unique(inputs,i))return Status::invalid_argument;
    }
    return Status::ok;
}
}
Status PcmStageOperation::validate_source_input(const NumericOperationInput& input) const noexcept {
    const auto node=definition_view(descriptor_->node);NumericBufferContract expected;
    if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    return plan_.kind==PcmStageKind::Sequence && input.port==context_sequence_ports::silence?scalar_source(input,expected,limits_):
        validate_frame_vector(input,node,plan_.input_rows,limits_,false);
}
namespace {
bool skipped_source(const NumericOperationInput& input,PcmStageKind kind,NumericOperationEvent event) noexcept {
    const bool source=input.port<(kind==PcmStageKind::Sequence?4:2);
    return !source || (event==NumericOperationEvent::EndOfStream && input.status==Status::unavailable);
}
}
Status PcmStageOperation::validate_source_inputs(std::span<const NumericOperationInput> inputs,bool& has_frame,NumericOperationEvent event) const noexcept {
    has_frame=false;const auto bound=bound_inputs(inputs,definition_view(descriptor_->node));if(bound!=Status::ok)return bound;
    for(const auto& input:inputs){
        if(skipped_source(input,plan_.kind,event))continue;
        const auto status=validate_source_input(input);if(status!=Status::ok)return status;
        has_frame|=input.port==0;
    }
    if(!has_frame)return event==NumericOperationEvent::EndOfStream?Status::ok:Status::unavailable;
    return source_pair(inputs);
}
Status PcmStageOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    PcmStageControls& controls,bool& has_frame,NumericOperationEvent event) const noexcept {
    if(!output_contracts_match(outputs) || !pcm_stage_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    auto status=validate_source_inputs(inputs,has_frame,event);if(status!=Status::ok)return status;
    status=read_controls(inputs,controls);if(status!=Status::ok)return status;
    return prepare_output_views(outputs);
}
}
