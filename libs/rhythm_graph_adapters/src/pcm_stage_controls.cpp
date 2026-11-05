// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status float_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    float value=0;const auto status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(!std::isfinite(value) || (input.port==12 && value<0))return Status::invalid_argument;
    if(input.port==10)controls.threshold=value;
    else if(input.port==11)controls.silence=value;
    else controls.compression=value;
    return Status::ok;
}
Status profile_control(std::uint8_t value,const PcmStagePlan& plan,PcmStageControls& controls) noexcept {
    if(value>9)return Status::invalid_argument;
    rhythm_reference_ports::OnsetDecisionConfig timing;
    if(!rhythm_reference_ports::onset_decision_parameters(rhythm_reference_ports::OnsetRecipe(value),
        plan.onset.spectrum.hop_samples,float(plan.sample_rate),timing))return Status::invalid_argument;
    controls.profile=value;return Status::ok;
}
Status byte_control(const NumericOperationInput& input,const PcmStagePlan& plan,PcmStageControls& controls) noexcept {
    std::uint8_t value=0;const auto status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(input.port==13){if(value>1)return Status::invalid_argument;controls.whiten=value!=0;return Status::ok;}
    if(plan.kind==PcmStageKind::Onset)return profile_control(value,plan,controls);
    if(value>1)return Status::invalid_argument;
    controls.poll_tatum=value!=0;return Status::ok;
}
Status unsigned_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    std::uint32_t value=0;const auto status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(input.port==17){if(value>8)return Status::invalid_argument;controls.window=value;return Status::ok;}
    if(!value || value>64)return Status::invalid_argument;
    controls.signature=value;return Status::ok;
}
Status onset_integer_control(const NumericOperationInput& input,PcmStageControls& controls) noexcept {
    std::uint64_t value=0;const auto status=load_element(input.value,{0,0,0},value);if(status!=Status::ok)return status;
    if(input.port==14)controls.delay_unsigned=value;else controls.minimum=value;return Status::ok;
}
Status integer_control(const NumericOperationInput& input,const PcmStagePlan& plan,PcmStageControls& controls) noexcept {
    if(plan.kind==PcmStageKind::Onset)return onset_integer_control(input,controls);
    if(input.port!=14)return unsigned_control(input,controls);
    std::int32_t value=0;const auto status=load_element(input.value,{0,0,0},value);
    if(status==Status::ok)controls.delay_signed=value;return status;
}
Status parse_control(const NumericOperationInput& input,const PcmStagePlan& plan,PcmStageControls& controls) noexcept {
    if(input.port>=10 && input.port<=12)return float_control(input,controls);
    if(input.port==13 || input.port==16)return byte_control(input,plan,controls);
    return integer_control(input,plan,controls);
}
Status coefficients_control(const NumericOperationInput& input,const domain::ResolvedProcessingNode& node,
    const PcmStagePlan& plan,const FrameLimits& limits,PcmStageControls& controls) noexcept {
    const auto status=validate_frame_vector(input,node,plan.onset.spectrum.window_samples,limits,false);
    if(status==Status::ok)controls.coefficients=input.contiguous_float32;return status;
}
Status validate_control(const NumericOperationInput& input,const domain::ResolvedProcessingNode& node,
    const PcmStagePlan& plan,const FrameLimits& limits,PcmStageControls& controls) noexcept {
    if(input.status!=Status::ok || !input.stamp.available)return Status::ok;
    NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    if(input.port==pcm_control_port)return validate_operation_control(input,expected,limits);
    if(input.port==18)return coefficients_control(input,node,plan,limits,controls);
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(DataFrameView{input.stamp,input.value},limits);
    return status==Status::ok?parse_control(input,plan,controls):status;
}
}
Status PcmStageOperation::read_controls(std::span<const NumericOperationInput> inputs,PcmStageControls& controls) const noexcept {
    const auto* poll=parameter(definition_view(descriptor_->node),"poll_tatum");
    if(poll){const auto* enabled=std::get_if<bool>(poll);if(!enabled)return Status::invalid_argument;controls.poll_tatum=*enabled;}
    for(const auto& input:inputs){
        if(input.port<10)continue;
        const auto status=validate_control(input,definition_view(descriptor_->node),plan_,limits_,controls);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
