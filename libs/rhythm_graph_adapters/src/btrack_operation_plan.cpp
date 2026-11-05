// SPDX-License-Identifier: AGPL-3.0-or-later
#include "btrack_operations.h"
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
    if(kind=="BTrackWindowProduct")return BTrackKind::WindowProduct;
    return BTrackKind::Onset;
}
bool canonical_parameters(const domain::ResolvedProcessingNode& node) noexcept {
    std::size_t rate=0,hop=0,frame=0,history=0;float alpha=0,tightness=0;
    return integer_parameter(node,"sample_rate",44100,rate) && rate==44100 && integer_parameter(node,"hop_samples",512,hop) && hop==512 &&
        integer_parameter(node,"frame_samples",1024,frame) && frame==1024 &&
        integer_parameter(node,"history_size",512,history) && history==512 &&
        scalar_parameter(node,"alpha",.9f,alpha) && alpha==.9f &&
        scalar_parameter(node,"tightness",5,tightness) && tightness==5;
}
bool window_parameters(const domain::ResolvedProcessingNode& node) noexcept {
    if(node.kind!="BTrackWindowProduct")return true;
    const auto* shape=parameter(node,"window_shape");
    return shape && std::get_if<std::string>(shape) && std::get<std::string>(*shape)=="hann";
}
bool cartesian_setting(const domain::ResolvedProcessingNode& node,bool& output) noexcept {
    const auto* value=parameter(node,"cartesian_input");if(!value)return true;
    const auto* flag=std::get_if<bool>(value);if(!flag || node.kind!="BTrackOnset")return false;
    output=*flag;return true;
}
BTrackShape input_shape(BTrackKind kind,domain::PortId port) noexcept {
    if(kind==BTrackKind::Onset || kind==BTrackKind::WindowProduct)return {Rank::Vector,1024};
    if(kind==BTrackKind::Resample || kind==BTrackKind::Comb)return {Rank::Vector,512};
    return port==1?BTrackShape{Rank::Vector,41}:BTrackShape{};
}
bool matches(const NumericContract& value,BTrackShape shape) noexcept {
    return value.dtype==shape.dtype && value.shape.rank==shape.rank && value.channel_order.size()==1 &&
        value.representation.encoding==StorageEncoding::Native && value.representation.normalization==Normalization::None &&
        value.shape.extents[0]==ExtentBounds{shape.count,shape.count} && value.shape.extents[1]==ExtentBounds{1,1};
}
bool port_matches(const domain::ResolvedProcessingNode& node,const BTrackPlan& plan,bool input,domain::PortId port,
    const ClockContract& clock) noexcept {
    NumericContract value;
    if(!numeric_port(node,input,port,value) || !equal_frame_clock(clock,value.clock))return false;
    auto shape=input?input_shape(plan.kind,port):btrack_shape(plan.kind,port,plan.cartesian_input);
    if(input && plan.kind==BTrackKind::History && value.dtype==DType::Float64)shape.dtype=DType::Float64;
    return matches(value,shape);
}
bool onset_coordinates(const domain::ResolvedProcessingNode& node) noexcept {
    NumericContract magnitude,phase;
    return numeric_port(node,true,0,magnitude) && numeric_port(node,true,1,phase) && phase.coordinates==magnitude.coordinates;
}
bool window_coordinates(const domain::ResolvedProcessingNode& node) noexcept {
    NumericContract input,output;
    if(!numeric_port(node,true,0,input) || !numeric_port(node,false,0,output))return false;
    for(const auto& axis:input.coordinates)if(axis.mode==CoordinateMode::ExplicitFloat64LE)return false;
    return input.coordinates==output.coordinates && input.shape.axes==output.shape.axes && input.unit==output.unit;
}
bool ports(const domain::ResolvedProcessingNode& node,const BTrackPlan& plan,bool input) noexcept {
    const auto kind=plan.kind;
    const auto values=input?node.inputs:node.outputs;
    const auto count=input?std::size_t(kind==BTrackKind::Onset || kind==BTrackKind::Decision?2:1):btrack_outputs(kind);
    NumericContract source;if(values.size()!=count || !numeric_port(node,true,0,source) || source.clock.sample_rate!=44100)return false;
    for(std::size_t i=0;i<count;++i)if(!port_matches(node,plan,input,domain::PortId(i),source.clock))return false;
    return !input || kind!=BTrackKind::Onset || onset_coordinates(node);
}
}
bool btrack_operation_kind(std::string_view kind) noexcept {
    return kind=="BTrackOnset" || kind=="BTrackHistory" || kind=="BTrackHistoryResample" ||
        kind=="BTrackCombObservation" || kind=="BTrackBeatDecision" || kind=="BTrackWindowProduct";
}
bool btrack_plan(const domain::ResolvedProcessingNode& node,BTrackPlan& plan) noexcept {
    plan={};if(!btrack_operation_kind(node.kind) || !canonical_parameters(node) || !window_parameters(node))return false;
    if(!cartesian_setting(node,plan.cartesian_input))return false;
    plan.kind=plan_kind(node.kind);
    return ports(node,plan,true) && ports(node,plan,false) &&
        (plan.kind!=BTrackKind::WindowProduct || window_coordinates(node));
}
std::size_t btrack_outputs(BTrackKind kind) noexcept {
    if(kind==BTrackKind::History)return 2;
    if(kind==BTrackKind::Comb)return 3;
    return kind==BTrackKind::Decision?8:1;
}
namespace {
BTrackShape decision_shape(domain::PortId port) noexcept {
    if(port==4)return {Rank::Vector,512};
    if(port==7)return {Rank::Vector,41};
    if(port==5 || port==6)return {Rank::Scalar,1,DType::Int64};
    return {};
}
bool valid_contracts(std::span<const domain::ProcessingPort> ports,const FrameLimits& limits) noexcept {
    for(const auto& port:ports)if(validate_contract(port.contract.view(),limits)!=Status::ok)return false;
    return true;
}
}
BTrackShape btrack_shape(BTrackKind kind,domain::PortId port,bool cartesian) noexcept {
    if(kind==BTrackKind::Onset && cartesian)return {Rank::Scalar,1,DType::Float64};
    if(kind==BTrackKind::WindowProduct)return {Rank::Vector,1024};
    if(kind==BTrackKind::Resample || (kind==BTrackKind::History && port==1))return {Rank::Vector,512};
    if(kind==BTrackKind::Comb)return {Rank::Vector,port==0?41u:port==1?128u:512u};
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
    if(plan.kind==BTrackKind::WindowProduct){plan.bytes=1024*(sizeof(double)+sizeof(float));return true;}
    if(plan.kind==BTrackKind::Onset)return plan.cartesian_input?
        rhythm_reference_ports::detail::PreparedPredictiveReferenceFrontend::complex_storage_bytes(1024,plan.bytes):
        dsp_kernels::PreparedSpectralDescriptor::storage_bytes(1024,plan.bytes);
    if(plan.kind==BTrackKind::History)return ScalarHistory::storage_bytes(512,plan.bytes);
    if(plan.kind==BTrackKind::Resample)return sinc_storage(plan,provider);
    if(!PreparedCombTempoTransition::storage_bytes(512,plan.bytes))return false;
    return plan.kind!=BTrackKind::Decision || (PreparedBeatPrediction::storage_bytes(512,43,5,.9f,extra) && numeric_operation_add_bytes(plan.bytes,extra));
}
NumericOperationPolicy btrack_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::ResetState;
    for(const auto& input:node.inputs)policy.inputs.push_back({input.id,NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    policy.output_values.assign(node.outputs.size(),ValueDomain::FiniteOnly);return policy;
}
domain::ProcessingSupport assess_btrack_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile,
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
