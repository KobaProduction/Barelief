// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adapter_operation.h"

namespace rhythm_graph_adapters::detail {
bool AdapterOperation::owned_resources(domain::ProcessingResources& resources) const noexcept {
    resources={};
    if(!descriptor_ || !dataflow_runtime::numeric_operation_descriptor_owned_bytes(*descriptor_,admission_,resources.plan_bytes) ||
       !dataflow_runtime::numeric_operation_add_bytes(resources.plan_bytes,object_bytes()))return false;
    resources.scratch_bytes=workspace_bytes();resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;return true;
}
namespace {
bool same_ports(std::span<const domain::ProcessingPort> left,std::span<const domain::ProcessingPort> right) noexcept {
    if(left.size()!=right.size())return false;
    for(std::size_t i=0;i<left.size();++i)
        if(left[i].id!=right[i].id || !signal_contracts::same_contract(left[i].contract.view(),right[i].contract.view()))return false;
    return true;
}
}
bool AdapterOperation::compatible(const AdapterOperation& other) const noexcept {
    if(!descriptor_ || !other.descriptor_)return false;
    const auto& a=descriptor_->node;const auto& b=other.descriptor_->node;
    return a.kind==b.kind && descriptor_->policy==other.descriptor_->policy &&
        domain::same_processing_parameters(a.parameters,b.parameters) && same_ports(a.inputs,b.inputs) && same_ports(a.outputs,b.outputs);
}
bool AdapterOperation::output_contracts_match(std::span<const dataflow_runtime::NumericOperationOutput> outputs) const noexcept {
    if(!descriptor_ || outputs.size()!=descriptor_->node.outputs.size())return false;
    for(const auto& expected:descriptor_->node.outputs){
        bool found=false;
        for(const auto& output:outputs)if(output.port==expected.id){
            if(!output.value.contract || !signal_contracts::same_contract(*output.value.contract,expected.contract.view()))return false;
            found=true;break;
        }
        if(!found)return false;
    }
    return true;
}
}
