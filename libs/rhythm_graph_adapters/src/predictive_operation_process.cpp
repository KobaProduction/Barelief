// SPDX-License-Identifier: AGPL-3.0-or-later
#include "predictive_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
Status validate_predictive_outputs(std::span<NumericOperationOutput> outputs,const FrameLimits& limits) noexcept {
    for(const auto& output:outputs){const auto status=validate_mutable(output.value,limits);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status validate_bound_inputs(std::span<const NumericOperationInput> inputs,const domain::ResolvedProcessingNode& node) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        NumericContract known;if(!numeric_port(node,true,inputs[i].port,known))return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(inputs[j].port==inputs[i].port)return Status::invalid_argument;
    }
    return Status::ok;
}
Status validate_predictive_source(const NumericOperationInput& input,const domain::ResolvedProcessingNode& node,const FrameLimits& limits) noexcept {
    NumericContract expected;if(!numeric_port(node,true,0,expected))return Status::incompatible_contract;
    if(expected.shape.rank==Rank::Vector)return validate_frame_vector(input,node,std::size_t(expected.shape.extents[0].maximum),limits,false);
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view({input.stamp,input.value},limits);
    return status==Status::ok?validate_values(input.value,ValueDomain::FiniteOnly,limits):status;
}
}
Status PredictiveOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    bool allow_empty,bool& has_frame,PredictiveWarmControls& controls) const noexcept {
    if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) ||
        !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    const auto node=definition_view(descriptor_->node);
    auto status=validate_bound_inputs(inputs,node);if(status!=Status::ok)return status;
    const auto* source=input_at(inputs,0);has_frame=source && source->status==Status::ok && source->stamp.available;
    status=read_controls(inputs,controls);if(status!=Status::ok)return status;
    if(!has_frame)return allow_empty?validate_predictive_outputs(outputs,limits_):Status::unavailable;
    status=validate_predictive_source(*source,node,limits_);
    return status==Status::ok?validate_predictive_outputs(outputs,limits_):status;
}
bool PredictiveOperation::calculate(const NumericOperationInput& input) noexcept {
    if(plan_.kind==PredictiveKind::Frontend)return std::get<PreparedPcmPredictiveOnset>(kernel_).process_hop(input.contiguous_float32,value_);
    if(plan_.kind==PredictiveKind::PcmTracker){
        if(!pcm_->process_hop(input.contiguous_float32,pcm_result_))return false;
        prediction_=pcm_result_.prediction;value_=pcm_result_.onset;return true;
    }
    if(plan_.kind==PredictiveKind::Comb)return std::get<PreparedCombTempoTransition>(kernel_).process(input.contiguous_float32,value_);
    float onset=0;if(load_element(input.value,{0,0,0},onset)!=Status::ok)return false;
    return plan_.kind==PredictiveKind::Prediction?std::get<PreparedBeatPrediction>(kernel_).process(onset,prediction_):
        std::get<PreparedPredictiveBeatTracker>(kernel_).process(onset,prediction_);
}
Status PredictiveOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    const auto* input=input_at(inputs,0);const auto stamp=operation_frame_stamp(input,false);
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,stamp);
    }
    PredictiveWarmControls controls;bool has_frame=false;
    auto status=validate_call(inputs,outputs,control.event==NumericOperationEvent::EndOfStream,has_frame,controls);if(status!=Status::ok)return status;
    if(control.reset)reset();
    if(!apply_controls(controls))return Status::invalid_argument;
    if(!has_frame)return Status::ok;
    if(!calculate(*input))return Status::invalid_argument;
    status=publish(outputs,stamp);if(status!=Status::ok)invalidate_outputs(outputs);return status;
}
Status PredictiveOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) const noexcept {
    auto status=validate_predictive_outputs(outputs,limits_);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        const auto shape=predictive_shape(plan_,output.port);output.value.actual={shape.rows,shape.columns};
        status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.stamp=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
}
