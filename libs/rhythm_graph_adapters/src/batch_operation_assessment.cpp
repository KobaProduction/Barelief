// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <dsp_kernels/kernel_support.h>
#include "operation_io.h"
#include "frame_operation_io.h"
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;using namespace dataflow_runtime;
namespace {
bool output_valid(const dataflow_runtime::NumericOperationPort& port,const BatchShape& shape,const FrameLimits& limits) noexcept {
    const auto& c=port.contract;
    if(validate_contract(c,limits)!=Status::ok)return false;
    return c.dtype==shape.type && c.shape.rank==shape.rank && c.plane_count==1 &&
        c.representation.encoding==StorageEncoding::Native && !c.representation.valid_bits && c.representation.layout==ChannelLayout::Planar && c.representation.normalization==Normalization::None &&
        c.shape.extents[0]==ExtentBounds{shape.variable?0:shape.rows,shape.rows} && c.shape.extents[1]==ExtentBounds{shape.columns,shape.columns};
}
bool pulse_storage(const BatchPlan& p,std::size_t& bytes) noexcept {
    switch(p.kind){
    case BatchKind::PulseTrain:return PreparedPulseTrainScoring::storage_bytes(p.rows,p.secondary,bytes);
    case BatchKind::LagConsensus:return PreparedLagTempoConsensus::storage_bytes(p.rate,p.classifier,bytes);
    default:return false;}
}
bool novelty_storage(const BatchPlan& p,std::size_t& bytes) noexcept {
    const auto& c=p.histogram.path;
    switch(p.kind){
    case BatchKind::FourierCandidates:return PreparedFourierTempoCandidates::storage_bytes(p.rows,{c.frequency_step,c.minimum_bpm,c.maximum_bpm,p.histogram.maximum_peaks},bytes);
    case BatchKind::WeightedHistogram:return PreparedWeightedTempoHistogram::storage_bytes(p.rows,p.secondary,{c.minimum_bpm,c.maximum_bpm,c.tolerance_percent,p.histogram.weight_by_magnitude},bytes);
    default:return false;}
}
bool histogram_storage(const BatchPlan& p,std::size_t& bytes) noexcept {
    const auto& c=p.histogram.path;
    switch(p.kind){
    case BatchKind::FramePath:return PreparedFrameTempoPath::storage_bytes(p.rows,c,bytes) && dsp_kernels::prepared_storage::add_array(bytes,p.columns/2,sizeof(InterpolatedPeak));
    default:return false;}
}
}
std::size_t batch_operation_input_count(BatchKind kind) noexcept {
    if(kind==BatchKind::FramePath)return 3;
    return kind==BatchKind::PulseTrain || kind==BatchKind::WeightedHistogram?2:1;
}
bool batch_operation_storage(const BatchPlan& p,const dataflow_runtime::NumericOperationNodeView&,std::size_t& bytes) noexcept {
    if(p.kind<=BatchKind::LagConsensus)return pulse_storage(p,bytes);
    return p.kind<=BatchKind::WeightedHistogram?novelty_storage(p,bytes):histogram_storage(p,bytes);
}
std::size_t batch_operation_object_size(BatchKind k) noexcept {
    if(k<=BatchKind::LagConsensus)return pulse_batch_object_size(k);
    return k<=BatchKind::WeightedHistogram?novelty_batch_object_size(k):histogram_batch_object_size(k);
}
namespace {
bool batch_ports(const dataflow_runtime::NumericOperationNodeView& n,const BatchPlan& plan,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<n.inputs.size();++k)if(n.inputs[k].id!=k || validate_contract(n.inputs[k].contract,limits)!=Status::ok)return false;
    for(std::size_t k=0;k<n.outputs.size();++k)if(n.outputs[k].id!=k || !output_valid(n.outputs[k],plan.shape[k],limits))return false;
    return true;
}
}
domain::ProcessingSupport assess_batch_operation(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& profile) noexcept {
    BatchPlan plan;domain::ProcessingSupport result;
    if(!batch_operation_plan(n,plan) || !batch_operation_shapes(n,plan) || n.inputs.size()!=batch_operation_input_count(plan.kind) || n.outputs.size()!=plan.outputs)return {};
    if(!batch_ports(n,plan,profile.admission.frame))return {};
    if(!numeric_operation_descriptor_required_bytes(n,profile.admission,result.resources.plan_bytes) || !numeric_operation_add_bytes(result.resources.plan_bytes,batch_operation_object_size(plan.kind)) || !batch_operation_storage(plan,n,result.resources.scratch_bytes))return {};
    // Reference processing stack plus this prepared object's cold temporary.
    // Heap payload is already counted above; object headers are not duplicated.
    result.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    if(!numeric_operation_add_bytes(result.resources.stack_bytes,batch_operation_object_size(plan.kind)))return {};
    result.supported=true;return result;
}
NumericOperationPolicy batch_operation_policy(const dataflow_runtime::NumericOperationNodeView& n,const BatchPlan& p){
    NumericOperationPolicy policy;policy.inputs.reserve(n.inputs.size());policy.output_values.reserve(p.outputs);
    if(p.kind==BatchKind::FramePath)policy.mode=NumericOperationMode::ContinuingStream;
    for(const auto& input:n.inputs)policy.inputs.push_back({input.id,NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    for(std::size_t i=0;i<p.outputs;++i)policy.output_values.push_back(p.shape[i].values);
    return policy;
}
std::unique_ptr<AdapterOperation> prepare_batch_operation(const dataflow_runtime::NumericOperationNode& node){
    const auto n=definition_view(node);BatchPlan p;if(!batch_operation_plan(n,p) || !batch_operation_shapes(n,p))return {};
    if(p.kind<=BatchKind::LagConsensus)return prepare_pulse_batch(n,p);
    return p.kind<=BatchKind::WeightedHistogram?prepare_novelty_batch(n,p):prepare_histogram_batch(n,p);
}
}
