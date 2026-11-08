// SPDX-License-Identifier: AGPL-3.0-or-later
#include "btrack_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
using namespace rhythm_reference_ports;
namespace {
Status output_views(std::span<NumericOperationOutput> outputs,const FrameLimits& limits) noexcept {
    for(const auto& output:outputs){const auto status=validate_mutable(output.value,limits);if(status!=Status::ok)return status;}
    return Status::ok;
}
Status scalar_value(const NumericBufferView& input,double& value) noexcept {
    if(input.contract->dtype==DType::Float64)return load_element(input,{0,0,0},value);
    float single=0;const auto status=load_element(input,{0,0,0},single);value=single;return status;
}
Status scalar_input(const NumericOperationInput& input,const dataflow_runtime::NumericOperationNodeView& node,const FrameLimits& limits,bool positive) noexcept {
    NumericBufferContract expected;if(!numeric_port(node,true,input.port,expected) || !input.value.contract ||
        !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(input.value,limits);
    if(status!=Status::ok)return status;
    double value=0;const auto loaded=scalar_value(input.value,value);
    return loaded!=Status::ok?loaded:!std::isfinite(value) || (positive && value<0)?Status::invalid_argument:Status::ok;
}
bool same_step(const dataflow_runtime::NumericOperationOrigin& a,const dataflow_runtime::NumericOperationOrigin& b) noexcept {
    return numeric_operation_same_step(a,b);
}
dataflow_runtime::NumericOperationOrigin feature_stamp(const NumericOperationInput* first) noexcept {
    return operation_point_stamp(first);
}
bool unique_inputs(std::span<const NumericOperationInput> inputs) noexcept {
    for(std::size_t i=0;i<inputs.size();++i)for(std::size_t j=0;j<i;++j)if(inputs[j].port==inputs[i].port)return false;
    return true;
}
Status bound_input(const NumericOperationInput& input,const dataflow_runtime::NumericOperationNodeView& node,
    const dataflow_runtime::NumericOperationOrigin& first,const BTrackPlan& plan,const FrameLimits& limits) noexcept {
    NumericBufferContract known;if(!numeric_port(node,true,input.port,known))return Status::incompatible_contract;
    if(input.status!=Status::ok)return Status::unavailable;
    if(!same_step(first,input.origin))return Status::incompatible_contract;
    const bool positive=plan.kind==BTrackKind::Decision && input.port==1;
    return known.shape.rank==Rank::Scalar?scalar_input(input,node,limits,plan.kind==BTrackKind::Decision):
        validate_frame_vector(input,node,std::size_t(known.shape.extents[0].maximum),limits,positive);
}
}
bool BTrackOperation::valid_buffers(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    return output_contracts_match(outputs) && disjoint_operation_buffers(inputs,outputs) && proof_buffers_disjoint(inputs,outputs);
}
Status BTrackOperation::validate(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    const auto node=definition_view(descriptor_->node);if(inputs.size()!=node.inputs.size())return Status::invalid_argument;
    const auto* first=input_at(inputs,0);if(!first || !unique_inputs(inputs))return Status::invalid_argument;
    for(const auto& input:inputs){
        const auto status=bound_input(input,node,first->origin,plan_,limits_);
        if(status!=Status::ok)return status;
    }
    return output_views(outputs,limits_);
}
bool BTrackOperation::calculate(std::span<const NumericOperationInput> inputs) noexcept {
    const auto* first=input_at(inputs,0);
    if(plan_.kind==BTrackKind::Threshold)return btrack_adaptive_threshold(first->contiguous_float32,std::span(resampled_).first(plan_.size));
    if(plan_.kind==BTrackKind::Response)return std::get<PreparedBTrackCombResponse>(kernel_).process(first->contiguous_float32,std::span(resampled_).first(128));
    if(plan_.kind==BTrackKind::Onset)return calculate_onset(inputs);
    if(plan_.kind==BTrackKind::Comb)return btrack_comb_observations(first->contiguous_float32,std::span(resampled_).first(41));
    if(plan_.kind==BTrackKind::Resample)return resample_history(first->contiguous_float32);
    if(plan_.kind==BTrackKind::History)return calculate_history(*first);
    return calculate_decision(inputs);
}
bool BTrackOperation::calculate_decision(std::span<const NumericOperationInput> inputs) noexcept {
    const auto* first=input_at(inputs,0);
    float value=0;if(load_element(first->value,{0,0,0},value)!=Status::ok)return false;
    return std::get<BTrackDecision>(kernel_).process(value,input_at(inputs,1)->contiguous_float32);
}
bool BTrackOperation::calculate_history(const NumericOperationInput& input) noexcept {
    float value=0;return load_element(input.value,{0,0,0},value)==Status::ok && push_history(value);
}
bool BTrackOperation::calculate_onset(std::span<const NumericOperationInput> inputs) noexcept {
    const auto first=input_at(inputs,0)->contiguous_float32,second=input_at(inputs,1)->contiguous_float32;
    return std::get<BTrackSourceOnset>(kernel_).process(first,second,scalar_);
}
bool BTrackSourceOnset::process(std::span<const float> real,std::span<const float> imaginary,float& output) noexcept {
    if(real.size()!=1024 || imaginary.size()!=1024)return false;
    for(std::size_t i=0;i<1024;++i){complex[2*i]=real[i];complex[2*i+1]=imaginary[i];}
    return kernel.process_complex(complex,output);
}
bool BTrackOperation::resample_history(std::span<const float> values) noexcept {
    std::size_t written=0;return resampler_->process_window(values,512,resampled_,512,written) && written==512;
}
bool BTrackOperation::push_history(float value) noexcept {
    scalar_=value;if(!std::isfinite(scalar_))return false;
    std::get<ScalarHistory>(kernel_).push(scalar_);return true;
}
Status BTrackOperation::disabled(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) const noexcept {
    auto status=output_views(outputs,limits_);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        const auto shape=btrack_shape(plan_.kind,output.port,plan_.size);output.value.actual={shape.count,1};
        status=fill_silence(output.value,limits_);if(status!=Status::ok)return status;
        output.origin=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
Status BTrackOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    if(!valid_buffers(inputs,outputs))return Status::invalid_argument;
    const auto stamp=feature_stamp(input_at(inputs,0));
    if(!control.enabled){
        const auto status=disabled(outputs,stamp);if(status!=Status::ok)invalidate_outputs(outputs);return status;
    }
    if(control.event==NumericOperationEvent::EndOfStream && inputs.empty())return Status::ok;
    return process_enabled(inputs,outputs,stamp,control);
}
Status BTrackOperation::process_enabled(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    const dataflow_runtime::NumericOperationOrigin& stamp,NumericOperationControl control) noexcept {
    const auto status=validate(inputs,outputs);if(status!=Status::ok)return status;
    if(control.reset)reset();
    if(!calculate(inputs)){reset();return Status::invalid_argument;}
    return finish(outputs,stamp);
}
Status BTrackOperation::finish(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    auto result=publish(outputs,stamp);
    for(const auto& output:outputs)if(result==Status::ok)result=validate_values(output.value.view(),ValueDomain::FiniteOnly,limits_);
    if(result!=Status::ok){invalidate_outputs(outputs);reset();}return result;
}
}
