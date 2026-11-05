// SPDX-License-Identifier: AGPL-3.0-or-later
#include "event_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status scalar_value(const NumericOperationInput& input,const NumericContract& expected,const FrameLimits& limits,float& peak,bool& silent) noexcept {
    if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    auto status=validate_view(DataFrameView{input.stamp,input.value},limits);if(status!=Status::ok)return status;
    if(input.port==0){
        status=load_element(input.value,{0,0,0},peak);
        return status==Status::ok && !std::isfinite(peak)?Status::invalid_argument:status;
    }
    std::uint8_t value=0;status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(value>1)return Status::invalid_argument;
    silent=value!=0;return Status::ok;
}
Status read_inputs(std::span<const NumericOperationInput> inputs,const domain::ResolvedProcessingNode& node,
    const EventPlan& plan,const FrameLimits& limits,float& peak,bool& silent) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        const auto& input=inputs[i];NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(inputs[j].port==input.port)return Status::invalid_argument;
        Status status=Status::ok;
        if(input.port==event_control_port)status=validate_operation_control(input,expected,limits);
        else if(plan.kind==EventOperationKind::Silence)status=validate_vector(input,plan.maximum,expected,limits);
        else status=scalar_value(input,expected,limits,peak,silent);
        if(status!=Status::ok)return status;
    }
    const auto* value=input_at(inputs,0);if(!value)return Status::unavailable;
    if(plan.kind!=EventOperationKind::Decision)return Status::ok;
    const auto* silence=input_at(inputs,1);if(!silence)return Status::unavailable;
    if(peak<0 || !equal_frame_identity(*value,*silence))return Status::invalid_argument;
    return Status::ok;
}
}
Status EventOperation::validate_event_outputs(std::span<NumericOperationOutput> outputs) const noexcept {
    for(const auto& output:outputs){
        if(!output.value.contract || output.value.contract->dtype!=event_output_dtype(plan_,output.port))return Status::incompatible_contract;
        const auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status EventOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    float& peak,bool& silent) const noexcept {
    if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    const auto status=read_inputs(inputs,definition_view(descriptor_->node),plan_,limits_,peak,silent);
    return status==Status::ok?validate_event_outputs(outputs):status;
}
}
