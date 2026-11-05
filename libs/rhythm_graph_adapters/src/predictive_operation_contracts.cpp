// SPDX-License-Identifier: AGPL-3.0-or-later
#include "predictive_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
PredictiveShape row(std::size_t rows,std::size_t columns=1) noexcept {return {DType::Float32,columns==1?Rank::Vector:Rank::Matrix,rows,columns};}
PredictiveShape prediction_shape(const PredictivePlan& plan,domain::PortId port) noexcept {
    if(!port)return {DType::UInt8};
    if(port<3)return {DType::Int64};
    if(plan.kind==PredictiveKind::Prediction && port==4)return row(plan.history);
    if(port==5 || port==6)return row(plan.history);
    if(port==7)return row(512);
    if(port==8)return row(128);
    return port>=9?row(41):PredictiveShape{};
}
PredictiveShape front_shape(const PredictivePlan& plan,domain::PortId port) noexcept {
    if(!port)return {};
    if(port==1)return row(plan.config.frame_samples,2);
    return row(plan.config.frame_samples);
}
bool valid_outputs(const domain::ResolvedProcessingNode& node,const PredictivePlan& plan) noexcept {
    if(node.outputs.size()!=predictive_output_count(plan.kind))return false;
    NumericContract source;if(!numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericContract output;if(!numeric_port(node,false,domain::PortId(i),output) || !equal_frame_clock(source.clock,output.clock))return false;
        output.clock={};if(!index_output_contract(output))return false;
        const auto shape=predictive_shape(plan,domain::PortId(i));
        if(output.dtype!=shape.dtype || output.shape.rank!=shape.rank || output.channel_order.size()!=1 ||
           output.shape.extents[0].maximum<shape.rows || output.shape.extents[0].minimum>shape.rows ||
           output.shape.extents[1].maximum<shape.columns || output.shape.extents[1].minimum>shape.columns)return false;
    }
    return true;
}
}
std::size_t predictive_output_count(PredictiveKind kind) noexcept {
    switch(kind){case PredictiveKind::Frontend:return 6;case PredictiveKind::Comb:return 5;
    case PredictiveKind::Prediction:return 5;case PredictiveKind::Tracker:return 11;case PredictiveKind::PcmTracker:return 17;}
    return 0;
}
PredictiveShape predictive_shape(const PredictivePlan& plan,domain::PortId port) noexcept {
    if(plan.kind==PredictiveKind::Frontend)return front_shape(plan,port);
    if(plan.kind==PredictiveKind::Comb)return port<2?PredictiveShape{}:row(port==2?128:41);
    if(plan.kind!=PredictiveKind::PcmTracker)return prediction_shape(plan,port);
    if(!port)return {};
    return port<12?prediction_shape(plan,domain::PortId(port-1)):front_shape(plan,domain::PortId(port-11));
}
domain::ProcessingSupport assess_predictive_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile,
    const dsp_kernels::ResamplerBackendFactory* provider) noexcept {
    domain::ProcessingSupport support;PredictivePlan plan;
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),profile.admission.frame)!=Status::ok)return {};
    if(!predictive_operation_plan(node,plan) || !predictive_operation_storage(plan,provider) || !valid_outputs(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(PredictiveOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    support.supported=true;return support;
}
NumericOperationPolicy predictive_operation_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;policy.match=NumericInputMatch::SessionSequence;
    policy.session=NumericInputSession::SharedProvenance;policy.arithmetic_failure=NumericOperationFailure::PreserveState;
    policy.inputs.reserve(node.inputs.size());
    for(const auto& input:node.inputs){
        const bool protocol=input.id==predictive_control_port;
        if(protocol)policy.control=input.id;
        policy.inputs.push_back({input.id,!input.id?NumericInputUse::Frame:protocol?NumericInputUse::Control:NumericInputUse::HeldValue,!input.id,ValueDomain::FiniteOnly});
    }
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
