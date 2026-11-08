// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status delay_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    std::int32_t value=0;const auto status=load_element(input.value,{0,0,0},value);
    if(status==Status::ok)controls.delay_signed=value;
    return status;
}
Status signature_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    std::uint32_t value=0;const auto status=load_element(input.value,{0,0,0},value);
    if(status!=Status::ok)return status;
    if(!value || value>64)return Status::invalid_argument;
    controls.signature=value;return Status::ok;
}
Status poll_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    std::uint8_t value=0;const auto status=load_element(input.value,{0,0,0},value);
    if(status!=Status::ok)return status;
    if(value>1)return Status::invalid_argument;
    controls.poll_tatum=value!=0;return Status::ok;
}
Status parse_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    if(input.port==context_sequence_ports::delay_control)return delay_control(input,controls);
    if(input.port==context_sequence_ports::signature_control)return signature_control(input,controls);
    return poll_control(input,controls);
}
Status validate_control(const NumericOperationInput& input,const NumericOperationNodeView& node,
    const FrameLimits& limits,PcmStageControls& controls) noexcept {
    if(input.status!=Status::ok)return Status::ok;
    NumericBufferContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    if(input.port==pcm_control_port)return validate_operation_control(input,expected,limits);
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(input.value,limits);
    return status==Status::ok?parse_control(input,controls):status;
}
}
Status PcmStageOperation::read_controls(std::span<const NumericOperationInput> inputs,PcmStageControls& controls) const noexcept {
    const auto* poll=parameter(definition_view(descriptor_->node),"poll_tatum");
    if(poll){const auto* enabled=std::get_if<bool>(poll);if(!enabled)return Status::invalid_argument;controls.poll_tatum=*enabled;}
    for(const auto& input:inputs){
        if(input.port<context_sequence_ports::delay_control)continue;
        const auto status=validate_control(input,definition_view(descriptor_->node),limits_,controls);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
