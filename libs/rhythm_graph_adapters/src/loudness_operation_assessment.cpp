// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "frame_operation_io.h"
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;using namespace dataflow_runtime;
namespace {
bool output_valid(const dataflow_runtime::NumericOperationPort& port,const BatchShape& shape,const FrameLimits& limits,bool variable_columns) noexcept {
    const auto& c=port.contract;
    return validate_contract(c,limits)==Status::ok && c.dtype==shape.type && c.shape.rank==shape.rank && c.plane_count==1 &&
        c.representation.encoding==StorageEncoding::Native && !c.representation.valid_bits && c.representation.layout==ChannelLayout::Planar && c.representation.normalization==Normalization::None &&
        c.shape.extents[0]==ExtentBounds{shape.variable?0:shape.rows,shape.rows} && c.shape.extents[1]==ExtentBounds{variable_columns?0:shape.columns,shape.columns};
}
}
bool loudness_operation_storage(const dataflow_runtime::NumericOperationNodeView& n,const LoudnessPlan& p,std::size_t& bytes) noexcept {
    const auto borders=loudness_borders(n,p.kind);
    switch(p.kind){
    case LoudnessKind::LoopFit:return PreparedLoopLengthConfidence::storage_bytes(p.rate,bytes);
    case LoudnessKind::Ratios:return PreparedSpectralBandRatios::storage_bytes(p.rows,p.rate,borders,bytes) && dsp_kernels::prepared_storage::add_array(bytes,p.bands,sizeof(float));
    case LoudnessKind::Frequency:return PreparedFrequencyBandEnergy::storage_bytes(p.rows,p.rate,borders,bytes) && dsp_kernels::prepared_storage::add_array(bytes,p.bands,sizeof(float));
    case LoudnessKind::Salience:return PreparedBeatBandSalience::storage_bytes(p.rows,p.columns,p.window,bytes);
    }
    return false;
}
std::size_t loudness_object_size(LoudnessKind k) noexcept {
    if(k==LoudnessKind::LoopFit)return loop_kernel_size(k);
    return k!=LoudnessKind::Salience?energy_kernel_size(k):rhythm_matrix_kernel_size(k);
}
namespace {
bool loudness_ports(const dataflow_runtime::NumericOperationNodeView& n,const LoudnessPlan& p,const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<n.inputs.size();++i)if(n.inputs[i].id!=i || validate_contract(n.inputs[i].contract,limits)!=Status::ok)return false;
    for(std::size_t i=0;i<n.outputs.size();++i)if(n.outputs[i].id!=i || !output_valid(n.outputs[i],p.shape[i],limits,p.kind==LoudnessKind::Salience && i==0))return false;
    return true;
}
}
domain::ProcessingSupport assess_loudness_operation(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& profile) noexcept {
    LoudnessPlan p;domain::ProcessingSupport out;
    if(!loudness_operation_plan(n,p) || n.outputs.size()!=p.outputs)return {};
    if(!loudness_ports(n,p,profile.admission.frame))return {};
    if(!numeric_operation_descriptor_required_bytes(n,profile.admission,out.resources.plan_bytes) ||
       !numeric_operation_add_bytes(out.resources.plan_bytes,loudness_object_size(p.kind)) || !loudness_operation_storage(n,p,out.resources.scratch_bytes))return {};
    out.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    if(!numeric_operation_add_bytes(out.resources.stack_bytes,loudness_object_size(p.kind)))return {};
    out.supported=true;return out;
}
NumericOperationPolicy loudness_operation_policy(const dataflow_runtime::NumericOperationNodeView& n,const LoudnessPlan& p){
    NumericOperationPolicy policy;policy.inputs.reserve(n.inputs.size());policy.output_values.reserve(p.outputs);
    for(const auto& input:n.inputs)policy.inputs.push_back({input.id,NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    for(std::size_t k=0;k<p.outputs;++k)policy.output_values.push_back(p.shape[k].values);
    return policy;
}
std::unique_ptr<AdapterOperation> prepare_loudness_operation(const dataflow_runtime::NumericOperationNode& node){
    const auto n=definition_view(node);LoudnessPlan p;if(!loudness_operation_plan(n,p))return {};
    if(p.kind==LoudnessKind::LoopFit)return prepare_loop_kernel(n,p);
    return p.kind!=LoudnessKind::Salience?prepare_energy_kernel(n,p):prepare_rhythm_matrix_kernel(n,p);
}
}
