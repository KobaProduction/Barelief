// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status read_chunk(std::span<const NumericOperationInput> inputs,std::size_t index,const dataflow_runtime::NumericOperationNodeView& node,
    const ContrastPlan& plan,const FrameLimits& limits,NumericOperationEvent event,bool& has_frame) noexcept {
    const auto& input=inputs[index];NumericBufferContract expected;
    if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    if(!operation_input_unique(inputs,index))return Status::invalid_argument;
    if(input.port==0 && event==NumericOperationEvent::EndOfStream && input.status==Status::unavailable)return Status::ok;
    const auto status=input.port==contrast_control_port?validate_operation_control(input,expected,limits):validate_vector(input,plan.maximum,expected,limits);
    if(status!=Status::ok)return status;
    if(input.port==0)has_frame=true;
    return Status::ok;
}
Status read_chunks(std::span<const NumericOperationInput> inputs,const dataflow_runtime::NumericOperationNodeView& node,
    const ContrastPlan& plan,const FrameLimits& limits,NumericOperationEvent event,bool& has_frame) noexcept {
    has_frame=false;
    for(std::size_t i=0;i<inputs.size();++i){const auto status=read_chunk(inputs,i,node,plan,limits,event,has_frame);if(status!=Status::ok)return status;}
    return has_frame || event==NumericOperationEvent::EndOfStream?Status::ok:Status::unavailable;
}
}
Status ContrastOperation::validate_contrast_outputs(std::span<NumericOperationOutput> outputs) const noexcept {
    for(const auto& output:outputs){
        if(!output.value.contract)return Status::invalid_argument;
        const auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status ContrastOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control,bool& has_frame) const noexcept {
    if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    const auto status=read_chunks(inputs,definition_view(descriptor_->node),plan_,limits_,control.event,has_frame);
    return status==Status::ok?validate_contrast_outputs(outputs):status;
}
}
