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
Status scalar_source(const NumericOperationInput& input,const NumericContract& expected,const FrameLimits& limits) noexcept {
    if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(DataFrameView{input.stamp,input.value},limits);if(status!=Status::ok)return status;
    if(input.port==0){
        float value=0;const auto loaded=load_element(input.value,{0,0,0},value);
        return loaded==Status::ok && !std::isfinite(value)?Status::invalid_argument:loaded;
    }
    std::uint8_t value=0;const auto loaded=load_element(input.value,{0,0,0},value);
    return loaded==Status::ok && value>1?Status::invalid_argument:loaded;
}
Status source_pair(std::span<const NumericOperationInput> inputs) noexcept {
    const auto* a=input_at(inputs,0);const auto* b=input_at(inputs,1);
    if(!a || !b || !a->stamp.available || !b->stamp.available)return Status::unavailable;
    return equal_frame_identity(*a,*b)?Status::ok:Status::incompatible_contract;
}
Status bound_inputs(std::span<const NumericOperationInput> inputs,const domain::ResolvedProcessingNode& node) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        NumericContract expected;if(!numeric_port(node,true,inputs[i].port,expected))return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(inputs[j].port==inputs[i].port)return Status::invalid_argument;
    }
    return Status::ok;
}
}
Status PcmStageOperation::validate_source_inputs(std::span<const NumericOperationInput> inputs,bool& has_frame,NumericOperationEvent event) const noexcept {
    has_frame=false;const auto node=definition_view(descriptor_->node);
    const auto bound=bound_inputs(inputs,node);if(bound!=Status::ok)return bound;
    for(const auto& input:inputs){
        const bool source=input.port==0 || (input.port==1 && plan_.kind==PcmStageKind::Sequence);
        if(!source)continue;
        if(event==NumericOperationEvent::EndOfStream && (input.status==Status::unavailable || !input.stamp.available))continue;
        NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
        const auto status=plan_.kind==PcmStageKind::Sequence?scalar_source(input,expected,limits_):
            validate_frame_vector(input,node,plan_.input_rows,limits_,false);
        if(status!=Status::ok)return status;
        if(input.port==0)has_frame=true;
    }
    if(!has_frame)return event==NumericOperationEvent::EndOfStream?Status::ok:Status::unavailable;
    return plan_.kind==PcmStageKind::Sequence?source_pair(inputs):Status::ok;
}
Status PcmStageOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    PcmStageControls& controls,bool& has_frame,NumericOperationEvent event) const noexcept {
    if(!output_contracts_match(outputs) || !pcm_stage_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    auto status=validate_source_inputs(inputs,has_frame,event);if(status!=Status::ok)return status;
    status=read_controls(inputs,controls);if(status!=Status::ok)return status;
    if(has_frame && plan_.kind!=PcmStageKind::Tracker && plan_.kind!=PcmStageKind::Sequence &&
        samples_>std::uint64_t(INT64_MAX)-plan_.onset.spectrum.hop_samples)return Status::invalid_argument;
    return prepare_output_views(outputs,has_frame);
}
}
