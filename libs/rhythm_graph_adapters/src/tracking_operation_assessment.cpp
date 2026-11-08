// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "frame_operation_io.h"
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
#include <rhythm_reference_ports/periodic_comb.h>
namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
using namespace dsp_kernels::prepared_storage;
std::span<const float> gains(const dataflow_runtime::NumericOperationNodeView& n) noexcept {const auto v=array_parameter(n,"gains");return v.empty()?std::span<const float>(default_tempo_band_gains):v;}
bool global_storage(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p,std::size_t& b) noexcept {
    switch(p.kind){
    case TrackingKind::InformationGain:return PreparedInformationGain::storage_bytes(p.rows,p.first,p.stop,b);
    case TrackingKind::Erb:return PreparedErbFilterBank::storage_bytes(p.rows,p.erb,b) && add_array(b,p.erb.bands,sizeof(float));
    default:return false;}
}
bool period_storage(const TrackingPlan& p,std::size_t& b) noexcept {
    switch(p.kind){
    case TrackingKind::PeriodPath:return PreparedPeriodPath::storage_bytes(p.rows,p.path,b) && add_array(b,p.rows,2*sizeof(float));
    case TrackingKind::Renewal:return PreparedRenewalBeatDecoder::storage_bytes(p.rows,p.counts[0],p.renewal,b) && add_array(b,p.rows,sizeof(std::uint8_t)+sizeof(float));
    case TrackingKind::Agreement:{std::size_t n=0;return PreparedBeatAgreement::storage_bytes(p.rows,p.columns,p.minimum_time,b) && mul_bytes(p.rows,p.columns,n) && add_array(b,n,sizeof(float)) && add_array(b,p.rows,sizeof(BeatSequenceView));}
    default:return false;}
}
bool legacy_storage(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& p,std::size_t& b) noexcept {
    const auto hints=array_parameter(n,"tempo_hints_seconds");
    switch(p.kind){
    case TrackingKind::BandFeatures:return PreparedBandTempoFeatures::storage_bytes(gains(n),p.frame_time,b) && add_array(b,p.rows,sizeof(float));
    case TrackingKind::PhaseTicks:return PreparedPeriodPhaseTicks::storage_bytes(p.rows,p.maximum_matches,p.maximum_ticks,p.ticks,b);
    case TrackingKind::HarmonicComb:return harmonic_comb_geometry(p.matrix?p.columns:p.rows,p.response_length,p.first,p.stop,p.harmonics) && add_array(b,p.response_length,sizeof(float)) && add_array(b,p.matrix?p.columns:0,sizeof(float));
    case TrackingKind::PhaseResponse:return p.response_length && p.harmonics && std::isfinite(p.period) && p.period>0 && add_array(b,p.response_length,sizeof(float));
    case TrackingKind::Silence:return p.leading<=p.rows && p.acquired<=p.rows-p.leading && PreparedFrameSilencePolicy::storage_bytes(p.rows,p.silence,b) && add_array(b,p.rows,sizeof(float));
    case TrackingKind::LagPool:return PreparedLagObservationPool::storage_bytes(p.maximum_rows,p.maximum_matches,p.maximum_matches,b);
    case TrackingKind::TickRefinement:return p.rows<=p.maximum_ticks && PreparedTickSequenceRefinement::storage_bytes(p.maximum_ticks,p.refinement,b);
    case TrackingKind::LagVote:return PreparedLagTempoVote::storage_bytes(p.rows,p.vote,b);
    default:return false;}
}
bool output_valid(const dataflow_runtime::NumericOperationPort& port,const BatchShape& shape,const FrameLimits& limits) noexcept {
    const auto& c=port.contract;if(validate_contract(c,limits)!=Status::ok)return false;
    return c.dtype==shape.type && c.shape.rank==shape.rank && c.plane_count==1 && c.representation.encoding==StorageEncoding::Native && !c.representation.valid_bits && c.representation.layout==ChannelLayout::Planar && c.representation.normalization==Normalization::None &&
        c.shape.extents[0]==ExtentBounds{shape.variable?0:shape.rows,shape.rows} && c.shape.extents[1]==ExtentBounds{shape.columns,shape.columns};
}
}
bool tracking_operation_storage(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& p,std::size_t& b) noexcept {
    b=0;if(p.kind==TrackingKind::InformationGain || p.kind==TrackingKind::Erb)return global_storage(n,p,b);
    if(p.kind>=TrackingKind::PeriodPath && p.kind<=TrackingKind::Agreement)return period_storage(p,b);
    return legacy_storage(n,p,b);
}
namespace {
bool tracking_ports_valid(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& plan,const FrameLimits& limits) noexcept {
    NumericBufferContract source;if(plan.frame && !numeric_port(n,true,0,source))return false;
    for(const auto& input:n.inputs)if(validate_contract(input.contract.view(),limits)!=Status::ok)return false;
    for(std::size_t i=0;i<n.outputs.size();++i)if(n.outputs[i].id!=i || !output_valid(n.outputs[i],plan.shape[i],limits))return false;
    return true;
}
bool tracking_resources(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& plan,
    const NumericOperationProfile& profile,domain::ProcessingSupport& out) noexcept {
    const auto object=tracking_operation_object_bytes(plan.kind);
    if(!numeric_operation_descriptor_required_bytes(n,profile.admission,out.resources.plan_bytes) ||
       !numeric_operation_add_bytes(out.resources.plan_bytes,object) || !tracking_operation_storage(n,plan,out.resources.scratch_bytes))return false;
    out.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    if(!numeric_operation_add_bytes(out.resources.stack_bytes,object))return false;
    out.supported=true;return true;
}
void tracking_value_policy(const dataflow_runtime::NumericOperationNodeView& node,const TrackingPlan& plan,NumericOperationPolicy& policy) {
    for(const auto& input:node.inputs)
        policy.inputs.push_back({input.id,input.id==tracking_control_port?NumericInputUse::Control:NumericInputUse::Frame,input.id!=tracking_control_port,ValueDomain::FiniteOnly});
    for(std::size_t i=0;i<plan.outputs;++i)policy.output_values.push_back(plan.shape[i].values);
}
}
domain::ProcessingSupport assess_tracking_operation(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& profile) noexcept {
    TrackingPlan plan;domain::ProcessingSupport out;
    if(!tracking_operation_plan(n,plan) || !tracking_operation_shapes(n,plan) || n.outputs.size()!=plan.outputs ||
       !tracking_ports_valid(n,plan,profile.admission.frame) || !tracking_resources(n,plan,profile,out))return {};
    return out;
}
NumericOperationPolicy tracking_operation_policy(const dataflow_runtime::NumericOperationNodeView& n){
    TrackingPlan plan;if(!tracking_operation_plan(n,plan) || !tracking_operation_shapes(n,plan))return {};
    NumericOperationPolicy policy;policy.mode=plan.frame?NumericOperationMode::ContinuingStream:NumericOperationMode::IndependentBatch;
    policy.match=NumericInputMatch::IndependentBatch;policy.arithmetic_failure=NumericOperationFailure::ResetState;
    if(plan.kind==TrackingKind::PhaseTicks){policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;}
    if(plan.control)policy.control=tracking_control_port;
    tracking_value_policy(n,plan,policy);return policy;
}
std::unique_ptr<AdapterOperation> prepare_tracking_operation(const dataflow_runtime::NumericOperationNode& node){
    const auto n=definition_view(node);TrackingPlan p;if(!tracking_operation_plan(n,p) || !tracking_operation_shapes(n,p))return {};
    if(p.kind==TrackingKind::InformationGain || p.kind==TrackingKind::Erb)return prepare_global_tracking(n,p);
    if(p.kind>=TrackingKind::PeriodPath && p.kind<=TrackingKind::Agreement)return prepare_period_tracking(n,p);
    return prepare_legacy_tracking(n,p);
}
std::size_t tracking_operation_object_bytes(TrackingKind k) noexcept {
    if(k==TrackingKind::InformationGain || k==TrackingKind::Erb)return global_tracking_object_bytes(k);
    return k>=TrackingKind::PeriodPath && k<=TrackingKind::Agreement?period_tracking_object_bytes(k):legacy_tracking_object_bytes(k);
}
} // namespace rhythm_graph_adapters::detail
