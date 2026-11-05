// SPDX-License-Identifier: AGPL-3.0-or-later
#include "event_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <algorithm>
#include <limits>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool coefficients(const domain::ResolvedProcessingNode& node,std::string_view key,std::array<float,3>& values) noexcept {
    if(!parameter(node,key))return true;
    const auto array=array_parameter(node,key);if(array.size()!=values.size())return false;
    std::copy(array.begin(),array.end(),values.begin());return true;
}
bool picker_plan(const domain::ResolvedProcessingNode& node,EventPlan& plan) noexcept {
    std::size_t statistic=0;plan.kind=EventOperationKind::Picker;
    if(!integer_parameter(node,"pre",1,plan.picker.pre) || !integer_parameter(node,"post",5,plan.picker.post) ||
       !scalar_parameter(node,"threshold",.1f,plan.picker.threshold) || !integer_parameter(node,"statistic",0,statistic) || statistic>1 ||
       !coefficients(node,"feedforward",plan.picker.feedforward) || !coefficients(node,"feedback",plan.picker.feedback))return false;
    plan.picker.statistic=ThresholdStatistic(statistic);
    return PreparedAdaptivePeakPicker::storage_bytes(plan.picker,plan.kernel_bytes);
}
bool decision_plan(const domain::ResolvedProcessingNode& node,EventPlan& plan) noexcept {
    std::size_t hop=512,delay=2201,minimum=2205;plan.kind=EventOperationKind::Decision;
    if(!integer_parameter(node,"hop_samples",512,hop) || !hop || hop>std::numeric_limits<std::uint32_t>::max() ||
       !integer_parameter(node,"delay_samples",2201,delay) || !integer_parameter(node,"minimum_interval_samples",2205,minimum))return false;
    plan.decision={std::uint32_t(hop),std::uint64_t(minimum),std::uint64_t(delay)};return true;
}
bool scalar_input(const NumericContract& contract,DType dtype) noexcept {
    return contract.dtype==dtype && contract.shape.rank==Rank::Scalar && contract.channel_order.size()==1 &&
        contract.representation.normalization==Normalization::None;
}
bool supported_input(const domain::ResolvedProcessingNode& node,const EventPlan& plan,domain::PortId port) noexcept {
    NumericContract input,magnitude;if(!numeric_port(node,true,port,input) || !numeric_port(node,true,0,magnitude))return false;
    if(port==event_control_port)return scalar_input(input,DType::UInt8);
    if(port==1)return plan.kind==EventOperationKind::Decision && scalar_input(input,DType::UInt8) && equal_frame_clock(input.clock,magnitude.clock);
    if(port!=0)return false;
    if(plan.kind!=EventOperationKind::Silence)return scalar_input(input,DType::Float32);
    return input.dtype==DType::Float32 && input.shape.rank==Rank::Vector && input.channel_order.size()==1 &&
        input.representation.encoding==StorageEncoding::Native && input.shape.extents[0].minimum>=1;
}
bool supported_inputs(const domain::ResolvedProcessingNode& node,const EventPlan& plan) noexcept {
    if(node.inputs.size()>(plan.kind==EventOperationKind::Decision?3:2))return false;
    NumericContract required;if(!numeric_port(node,true,0,required) ||
        (plan.kind==EventOperationKind::Decision && !numeric_port(node,true,1,required)))return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        if(!supported_input(node,plan,node.inputs[i].id))return false;
        for(std::size_t j=0;j<i;++j)if(node.inputs[i].id==node.inputs[j].id)return false;
    }
    return true;
}
}
bool event_operation_kind(std::string_view kind) noexcept {
    return kind=="AdaptivePeakPicker" || kind=="OnsetDecision" || kind=="PcmSilence";
}
bool plan_event_operation(const domain::ResolvedProcessingNode& node,EventPlan& plan) noexcept {
    plan={};bool ready=false;
    if(node.kind=="AdaptivePeakPicker")ready=picker_plan(node,plan);
    else if(node.kind=="OnsetDecision")ready=decision_plan(node,plan);
    else if(node.kind=="PcmSilence"){
        plan.kind=EventOperationKind::Silence;ready=scalar_parameter(node,"threshold_db",-70,plan.silence_db) &&
            vector_capacity(node,domain::PortDirection::Input,0,plan.maximum) && plan.maximum;
    }
    return ready && supported_inputs(node,plan);
}
std::size_t event_output_count(const EventPlan& plan) noexcept {
    return plan.kind==EventOperationKind::Picker?5:plan.kind==EventOperationKind::Decision?3:1;
}
DType event_output_dtype(const EventPlan& plan,domain::PortId port) noexcept {
    if(plan.kind==EventOperationKind::Silence || (plan.kind==EventOperationKind::Picker && port==1) ||
        (plan.kind==EventOperationKind::Decision && port==2))return DType::UInt8;
    return plan.kind==EventOperationKind::Decision && port==1?DType::UInt64:DType::Float32;
}
}
