// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operations.h"
#include "operation_io.h"
#include "parameter_values.h"
#include "frame_operation_io.h"
#include <algorithm>
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status read_byte_control(const NumericOperationInput& input,SpectralWarmControls& controls) noexcept {
    std::uint8_t value=0;const auto status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(value>(input.port==10?1:3))return Status::invalid_argument;
    if(input.port==10)controls.whiten=value!=0;
    return Status::ok;
}
Status read_float_control(const NumericOperationInput& input,SpectralWarmControls& controls) noexcept {
    float value=0;const auto status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(!std::isfinite(value) || (input.port==11?value<0:value<=0))return Status::invalid_argument;
    if(input.port==11)controls.compression=value;
    else if(input.port==12)controls.floor=value;
    else if(input.port==13)controls.relax_time=value;
    else return Status::invalid_argument;
    return Status::ok;
}
Status read_control_value(const NumericOperationInput& input,SpectralWarmControls& controls) noexcept {
    return input.port==spectral_control_port || input.port==10?read_byte_control(input,controls):read_float_control(input,controls);
}
Status validate_control(const NumericOperationInput& input,const NumericContract& expected,const FrameLimits& limits,
    SpectralWarmControls& controls) noexcept {
    if(input.status!=Status::ok || !input.stamp.available)return Status::ok;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(DataFrameView{input.stamp,input.value},limits);
    return status==Status::ok?read_control_value(input,controls):status;
}
}
Status SpectralOperation::validate_frame_input(const NumericOperationInput& input) const noexcept {
    return validate_frame_vector(input,definition_view(descriptor_->node),plan_.bins,limits_,input.port==0);
}
Status SpectralOperation::read_controls(std::span<const NumericOperationInput> inputs,SpectralWarmControls& controls) const noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        const auto& input=inputs[i];NumericContract expected;
        if(!spectral_port(definition_view(descriptor_->node),true,input.port,expected))return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(inputs[j].port==input.port)return Status::invalid_argument;
        if(input.port<2)continue;
        const auto status=validate_control(input,expected,limits_,controls);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status SpectralOperation::validate_phase_pair(std::span<const NumericOperationInput> inputs,const NumericOperationInput& magnitude) const noexcept {
    if(!plan_.phase_input())return Status::ok;
    const auto* phase=input_at(inputs,1);if(!phase)return Status::unavailable;
    const auto status=validate_frame_input(*phase);if(status!=Status::ok)return status;
    return equal_frame_identity(magnitude,*phase)?Status::ok:Status::incompatible_contract;
}
Status SpectralOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    SpectralWarmControls& controls) const noexcept {
    if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs))return Status::invalid_argument;
    const auto* magnitude=input_at(inputs,0);if(!magnitude)return Status::unavailable;
    auto status=validate_frame_input(*magnitude);if(status!=Status::ok)return status;
    status=validate_phase_pair(inputs,*magnitude);if(status!=Status::ok)return status;
    status=read_controls(inputs,controls);if(status!=Status::ok)return status;
    if(!proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    return validate_outputs(outputs,limits_);
}
}
