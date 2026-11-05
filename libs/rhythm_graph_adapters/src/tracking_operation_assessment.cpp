// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
#include "frame_operation_io.h"
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
#include <rhythm_reference_ports/periodic_comb.h>
namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
using namespace dsp_kernels::prepared_storage;
std::span<const float> gains(const domain::ResolvedProcessingNode& n) noexcept {const auto v=array_parameter(n,"gains");return v.empty()?std::span<const float>(default_tempo_band_gains):v;}
bool global_storage(const domain::ResolvedProcessingNode&,const TrackingPlan& p,std::size_t& b) noexcept {
    if(p.global_recipe)return PreparedGlobalOnsetRecipe::storage_bytes(p.rows,p.global,b);
    switch(p.kind){
    case TrackingKind::InformationGain:return PreparedInformationGain::storage_bytes(p.rows,p.first,p.stop,b);
    case TrackingKind::BandEmphasis:return PreparedBandBeatEmphasis::storage_bytes(p.rows,p.emphasis,b) && add_array(b,p.rows,sizeof(float));
    case TrackingKind::Erb:return PreparedErbFilterBank::storage_bytes(p.rows,p.erb,b) && add_array(b,p.erb.bands,sizeof(float));
    case TrackingKind::PcmBeat:return PreparedPcmBeatRecipe::storage_bytes(p.rows,p.pcm,b);
    default:return false;}
}
bool period_storage(const TrackingPlan& p,std::size_t& b) noexcept {
    switch(p.kind){
    case TrackingKind::Threshold:return PreparedMovingThreshold::storage_bytes(p.rows,p.half,b) && add_array(b,p.rows,sizeof(float));
    case TrackingKind::PeriodPath:return PreparedPeriodPath::storage_bytes(p.rows,p.path,b) && add_array(b,p.rows,2*sizeof(float));
    case TrackingKind::Renewal:return PreparedRenewalBeatDecoder::storage_bytes(p.rows,p.counts[0],p.renewal,b) && add_array(b,p.rows,sizeof(std::uint8_t));
    case TrackingKind::PeriodBeat:{std::size_t n=0;return PreparedPeriodBeatTracker::storage_bytes(p.rows,p.beat,b) && dsp_kernels::endpoint_expansion_size(p.rows,p.beat.upsampling,n) && add_array(b,n,sizeof(float)+sizeof(std::uint8_t));}
    case TrackingKind::Agreement:{std::size_t n=0;return PreparedBeatAgreement::storage_bytes(p.rows,p.columns,p.minimum_time,b) && mul_bytes(p.rows,p.columns,n) && add_array(b,n,sizeof(float)) && add_array(b,p.rows,sizeof(BeatSequenceView));}
    case TrackingKind::MultiBeat:return PreparedMultiFeatureBeatTracker::storage_bytes(p.counts,p.multi,b);
    default:return false;}
}
bool legacy_storage(const domain::ResolvedProcessingNode& n,const TrackingPlan& p,std::size_t& b) noexcept {
    const auto hints=array_parameter(n,"tempo_hints_seconds");
    switch(p.kind){
    case TrackingKind::BandFeatures:return PreparedBandTempoFeatures::storage_bytes(gains(n),p.frame_time,b) && add_array(b,p.rows,sizeof(float));
    case TrackingKind::CombPeriod:return PreparedCombPeriodPhase::storage_bytes(p.comb,hints,b);
    case TrackingKind::FeaturePeriod:return PreparedFeaturePeriodTracker::storage_bytes(p.rows,p.legacy.evaluation_hop,p.comb,hints,b);
    case TrackingKind::PhaseTicks:return PreparedPeriodPhaseTicks::storage_bytes(p.rows,p.maximum_matches,p.maximum_ticks,p.ticks,b);
    case TrackingKind::HarmonicComb:return harmonic_comb_geometry(p.rows,p.response_length,p.first,p.stop,p.harmonics) && add_array(b,p.response_length,sizeof(float));
    case TrackingKind::PhaseResponse:return p.response_length && p.harmonics && std::isfinite(p.period) && p.period>0 && add_array(b,p.response_length,sizeof(float));
    case TrackingKind::Silence:return p.leading<=p.rows && p.acquired<=p.rows-p.leading && PreparedFrameSilencePolicy::storage_bytes(p.rows,p.silence,b) && add_array(b,p.rows,sizeof(float));
    case TrackingKind::LagPool:return PreparedLagObservationPool::storage_bytes(p.maximum_rows,p.maximum_matches,p.maximum_matches,b);
    case TrackingKind::TickRefinement:return p.rows<=p.maximum_ticks && PreparedTickSequenceRefinement::storage_bytes(p.maximum_ticks,p.refinement,b);
    case TrackingKind::LagVote:return PreparedLagTempoVote::storage_bytes(p.rows,p.vote,b);
    case TrackingKind::Legacy:return PreparedLegacyRhythmRecipe::storage_bytes({p.rows,p.maximum_ticks,p.maximum_matches},p.legacy,hints,b);
    default:return false;}
}
bool output_valid(const domain::ProcessingPort& port,const BatchShape& shape,const NumericContract* source,const FrameLimits& limits) noexcept {
    const auto view=port.contract.view();const auto* c=std::get_if<NumericContract>(&view);if(!c || validate_contract(view,limits)!=Status::ok)return false;
    auto index=*c;if(source){if(!equal_frame_clock(c->clock,source->clock))return false;index.clock={};}
    return index_output_contract(index) && c->dtype==shape.type && c->shape.rank==shape.rank && c->channel_order.size()==1 && c->representation.encoding==StorageEncoding::Native && !c->representation.valid_bits && c->representation.layout==ChannelLayout::Planar && c->representation.normalization==Normalization::None &&
        c->shape.extents[0]==ExtentBounds{shape.variable?0:shape.rows,shape.rows} && c->shape.extents[1]==ExtentBounds{shape.columns,shape.columns};
}
}
bool tracking_operation_storage(const domain::ResolvedProcessingNode& n,const TrackingPlan& p,std::size_t& b) noexcept {
    b=0;if(p.global_recipe || p.kind==TrackingKind::InformationGain || p.kind==TrackingKind::BandEmphasis || p.kind==TrackingKind::Erb || p.kind==TrackingKind::PcmBeat)return global_storage(n,p,b);
    if(p.kind>=TrackingKind::Threshold && p.kind<=TrackingKind::MultiBeat)return period_storage(p,b);
    return legacy_storage(n,p,b);
}
domain::ProcessingSupport assess_tracking_operation(const domain::ResolvedProcessingNode& n,const NumericRuntimeProfile& profile) noexcept {
    TrackingPlan p;domain::ProcessingSupport out;if(!tracking_operation_plan(n,p) || !tracking_operation_shapes(n,p) || n.outputs.size()!=p.outputs)return {};
    NumericContract source;if(p.frame && !numeric_port(n,true,0,source))return {};
    for(const auto& input:n.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(std::size_t i=0;i<n.outputs.size();++i)if(n.outputs[i].id!=i || !output_valid(n.outputs[i],p.shape[i],p.frame?&source:nullptr,profile.admission.frame))return {};
    if(!numeric_operation_descriptor_required_bytes(n,profile.admission,out.resources.plan_bytes) || !numeric_operation_add_bytes(out.resources.plan_bytes,tracking_operation_object_bytes(p.kind,p.global_recipe)) || !tracking_operation_storage(n,p,out.resources.scratch_bytes))return {};
    out.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;if(!numeric_operation_add_bytes(out.resources.stack_bytes,tracking_operation_object_bytes(p.kind,p.global_recipe)))return {};
    out.supported=true;return out;
}
NumericOperationPolicy tracking_operation_policy(const domain::ResolvedProcessingNode& n){
    TrackingPlan p;if(!tracking_operation_plan(n,p) || !tracking_operation_shapes(n,p))return {};
    NumericOperationPolicy out;out.mode=(p.frame || p.kind==TrackingKind::Legacy)?NumericOperationMode::ContinuingStream:NumericOperationMode::IndependentBatch;
    out.match=NumericInputMatch::IndependentBatch;out.arithmetic_failure=NumericOperationFailure::ResetState;
    if(p.kind==TrackingKind::PhaseTicks){out.match=NumericInputMatch::SessionSequence;out.session=NumericInputSession::SharedProvenance;}
    if(p.control)out.control=tracking_control_port;
    for(const auto& in:n.inputs)out.inputs.push_back({in.id,in.id==tracking_control_port?NumericInputUse::Control:NumericInputUse::Frame,in.id!=tracking_control_port,ValueDomain::FiniteOnly});
    for(std::size_t i=0;i<p.outputs;++i)out.output_values.push_back(p.shape[i].values);return out;
}
std::unique_ptr<AdapterOperation> prepare_tracking_operation(const domain::ConfiguredProcessingNode& node){
    const auto n=definition_view(node);TrackingPlan p;if(!tracking_operation_plan(n,p) || !tracking_operation_shapes(n,p))return {};
    if(p.global_recipe || p.kind==TrackingKind::InformationGain || p.kind==TrackingKind::BandEmphasis || p.kind==TrackingKind::Erb || p.kind==TrackingKind::PcmBeat)return prepare_global_tracking(n,p);
    if(p.kind>=TrackingKind::Threshold && p.kind<=TrackingKind::MultiBeat)return prepare_period_tracking(n,p);
    return prepare_legacy_tracking(n,p);
}
std::size_t tracking_operation_object_bytes(TrackingKind k,bool global) noexcept {
    if(global || k==TrackingKind::InformationGain || k==TrackingKind::BandEmphasis || k==TrackingKind::Erb || k==TrackingKind::PcmBeat)return global_tracking_object_bytes(k,global);
    return k>=TrackingKind::Threshold && k<=TrackingKind::MultiBeat?period_tracking_object_bytes(k):legacy_tracking_object_bytes(k);
}
} // namespace rhythm_graph_adapters::detail
