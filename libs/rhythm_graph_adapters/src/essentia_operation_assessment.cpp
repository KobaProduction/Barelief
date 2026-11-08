// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_operations.h"
#include <dsp_kernels/kernel_support.h>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
bool valid_output(const dataflow_runtime::NumericOperationPort& port,const BatchShape& shape,const FrameLimits& limits) noexcept {
    const auto& c=port.contract;
    return validate_contract(c,limits)==Status::ok &&
        c.dtype==shape.type && c.shape.rank==shape.rank && c.plane_count==1 &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        c.representation.normalization==Normalization::None && !c.representation.valid_bits &&
        c.shape.extents[0]==ExtentBounds{shape.variable?0:shape.rows,shape.rows} &&
        c.shape.extents[1]==ExtentBounds{shape.columns,shape.columns};
}
bool valid_ports(const dataflow_runtime::NumericOperationNodeView& n,const EssentiaPlan& p,const FrameLimits& limits) noexcept {
    for(const auto& input:n.inputs)if(validate_contract(input.contract.view(),limits)!=Status::ok)return false;
    for(std::size_t i=0;i<p.outputs;++i)
        if(n.outputs[i].id!=i || !valid_output(n.outputs[i],p.shapes[i],limits))return false;
    return true;
}
}
domain::ProcessingSupport assess_essentia_operation(const dataflow_runtime::NumericOperationNodeView& n,
    const NumericOperationProfile& profile) noexcept {
    EssentiaPlan p;domain::ProcessingSupport out;
    if(!essentia_operation_plan(n,p) || n.outputs.size()!=p.outputs || !valid_ports(n,p,profile.admission.frame))return {};
    if(!numeric_operation_descriptor_required_bytes(n,profile.admission,out.resources.plan_bytes) ||
       !numeric_operation_add_bytes(out.resources.plan_bytes,essentia_operation_object_bytes(p.kind)) ||
       !essentia_operation_storage(p,out.resources.scratch_bytes))return {};
    out.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    if(!numeric_operation_add_bytes(out.resources.stack_bytes,essentia_operation_object_bytes(p.kind)))return {};
    out.supported=true;return out;
}
NumericOperationPolicy essentia_operation_policy(const dataflow_runtime::NumericOperationNodeView& n) {
    EssentiaPlan p;if(!essentia_operation_plan(n,p))return {};
    NumericOperationPolicy out;out.mode=NumericOperationMode::IndependentBatch;
    out.match=NumericInputMatch::SessionSequence;out.session=NumericInputSession::SharedProvenance;
    out.arithmetic_failure=NumericOperationFailure::ResetState;
    out.inputs.reserve(p.inputs);
    for(const auto& in:n.inputs)out.inputs.push_back({in.id,NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    out.output_values.assign(p.outputs,ValueDomain::FiniteOnly);return out;
}
}
