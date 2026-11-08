// SPDX-License-Identifier: AGPL-3.0-or-later
#include "btrack_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
BTrackKind plan_kind(std::string_view kind) noexcept {
    if(kind=="BTrackHistory")return BTrackKind::History;
    if(kind=="BTrackHistoryResample")return BTrackKind::Resample;
    if(kind=="BTrackCombObservation")return BTrackKind::Comb;
    if(kind=="BTrackBeatDecision")return BTrackKind::Decision;
    if(kind=="BTrackAdaptiveThreshold")return BTrackKind::Threshold;
    if(kind=="BTrackCombResponse")return BTrackKind::Response;
    return BTrackKind::Onset;
}
bool canonical_parameters(const dataflow_runtime::NumericOperationNodeView& node) noexcept {
    std::size_t rate=0,hop=0,frame=0,history=0;float alpha=0,tightness=0;
    return integer_parameter(node,"sample_rate",44100,rate) && rate==44100 && integer_parameter(node,"hop_samples",512,hop) && hop==512 &&
        integer_parameter(node,"frame_samples",1024,frame) && frame==1024 &&
        integer_parameter(node,"history_size",512,history) && history==512 &&
        scalar_parameter(node,"alpha",.9f,alpha) && alpha==.9f &&
        scalar_parameter(node,"tightness",5,tightness) && tightness==5;
}
BTrackShape input_shape(const BTrackPlan& plan,domain::PortId port) noexcept {
    const auto kind=plan.kind;
    if(kind==BTrackKind::Onset)return {Rank::Vector,1024};
    if(kind==BTrackKind::Threshold)return {Rank::Vector,plan.size};
    if(kind==BTrackKind::Comb)return {Rank::Vector,128};
    if(kind==BTrackKind::Resample || kind==BTrackKind::Response)return {Rank::Vector,512};
    return port==1?BTrackShape{Rank::Vector,41}:BTrackShape{};
}
bool matches(const NumericBufferContract& value,BTrackShape shape) noexcept {
    return value.dtype==shape.dtype && value.shape.rank==shape.rank && value.plane_count==1 &&
        value.representation.encoding==StorageEncoding::Native && value.representation.normalization==Normalization::None &&
        value.shape.extents[0]==ExtentBounds{shape.count,shape.count} && value.shape.extents[1]==ExtentBounds{1,1};
}
bool port_matches(const dataflow_runtime::NumericOperationNodeView& node,const BTrackPlan& plan,bool input,domain::PortId port) noexcept {
    NumericBufferContract value;
    if(!numeric_port(node,input,port,value))return false;
    auto shape=input?input_shape(plan,port):btrack_shape(plan.kind,port,plan.size);
    return matches(value,shape);
}

bool ports(const dataflow_runtime::NumericOperationNodeView& node,const BTrackPlan& plan,bool input) noexcept {
    const auto kind=plan.kind;
    const auto values=input?node.inputs:node.outputs;
    const auto count=input?std::size_t(kind==BTrackKind::Onset || kind==BTrackKind::Decision?2:1):btrack_outputs(kind);
    NumericBufferContract source;if(values.size()!=count || !numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<count;++i)if(!port_matches(node,plan,input,domain::PortId(i)))return false;
    return true;
}
}
bool btrack_operation_kind(std::string_view kind) noexcept {
    return kind=="BTrackOnset" || kind=="BTrackHistory" || kind=="BTrackHistoryResample" ||
        kind=="BTrackCombObservation" || kind=="BTrackBeatDecision" ||
        kind=="BTrackAdaptiveThreshold" || kind=="BTrackCombResponse";
}
bool btrack_plan(const dataflow_runtime::NumericOperationNodeView& node,BTrackPlan& plan) noexcept {
    plan={};if(!btrack_operation_kind(node.kind) || !canonical_parameters(node))return false;
    if(!integer_parameter(node,"size",512,plan.size) || (plan.size!=128 && plan.size!=512))return false;
    plan.kind=plan_kind(node.kind);
    return ports(node,plan,true) && ports(node,plan,false);
}
std::size_t btrack_outputs(BTrackKind kind) noexcept {
    if(kind==BTrackKind::History)return 2;
    return kind==BTrackKind::Decision?8:1;
}
namespace {
BTrackShape decision_shape(domain::PortId port) noexcept {
    if(port==4)return {Rank::Vector,512};
    if(port==7)return {Rank::Vector,41};
    if(port==5 || port==6)return {Rank::Scalar,1,DType::Int64};
    return {};
}
bool valid_contracts(dataflow_runtime::NumericOperationPorts ports,const FrameLimits& limits) noexcept {
    for(const auto& port:ports)if(validate_contract(port.contract.view(),limits)!=Status::ok)return false;
    return true;
}
}
BTrackShape btrack_shape(BTrackKind kind,domain::PortId port,std::size_t size) noexcept {
    if(kind==BTrackKind::Onset)return {Rank::Scalar,1,DType::Float64};
    if(kind==BTrackKind::Threshold)return {Rank::Vector,size};
    if(kind==BTrackKind::Response)return {Rank::Vector,128};
    if(kind==BTrackKind::Resample || (kind==BTrackKind::History && port==1))return {Rank::Vector,512};
    if(kind==BTrackKind::Comb)return {Rank::Vector,41};
    if(kind==BTrackKind::Decision)return decision_shape(port);
    return {};
}
namespace {
bool sinc_storage(BTrackPlan& plan,const dsp_kernels::ResamplerBackendFactory* provider) noexcept {
        dsp_kernels::ResamplerResources resources;
        if(!provider || !dsp_kernels::PreparedResampler::window_resource_requirements(btrack_sinc_config(),provider,resources))return false;
        plan.bytes=resources.owned_bytes;return true;
}
}
bool btrack_storage(BTrackPlan& plan,const dsp_kernels::ResamplerBackendFactory* provider) noexcept {
    using namespace rhythm_reference_ports;std::size_t extra=0;
    if(plan.kind==BTrackKind::Threshold || plan.kind==BTrackKind::Response || plan.kind==BTrackKind::Comb){plan.bytes=0;return true;}
    if(plan.kind==BTrackKind::Onset)return rhythm_reference_ports::detail::PreparedPredictiveReferenceFrontend::complex_storage_bytes(1024,plan.bytes);
    if(plan.kind==BTrackKind::History)return ScalarHistory::storage_bytes(512,plan.bytes);
    if(plan.kind==BTrackKind::Resample)return sinc_storage(plan,provider);
    if(!PreparedCombTempoTransition::storage_bytes(512,plan.bytes))return false;
    return plan.kind!=BTrackKind::Decision || (PreparedBeatPrediction::storage_bytes(512,43,5,.9f,extra) && numeric_operation_add_bytes(plan.bytes,extra));
}
NumericOperationPolicy btrack_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::ResetState;
    for(const auto& input:node.inputs)policy.inputs.push_back({input.id,NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    policy.output_values.assign(node.outputs.size(),ValueDomain::FiniteOnly);return policy;
}
domain::ProcessingSupport assess_btrack_operation(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile,
    const dsp_kernels::ResamplerBackendFactory* provider) noexcept {
    BTrackPlan plan;domain::ProcessingSupport support;
    if(!valid_contracts(node.inputs,profile.admission.frame) || !valid_contracts(node.outputs,profile.admission.frame))return {};
    if(!btrack_plan(node,plan) || !btrack_storage(plan,provider) ||
        !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
        !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(BTrackOperation)))return {};
    support.resources.scratch_bytes=plan.bytes;support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    support.supported=true;return support;
}
}
