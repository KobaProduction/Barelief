// SPDX-License-Identifier: AGPL-3.0-or-later
#include "metadata_policy.h"
#include "essentia_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
void essentia_meanings(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    for(const auto& port:node.outputs){
        NumericOperationMeaningRule rule;rule.direction=domain::PortDirection::Output;rule.port=port.id;
        rule.units.push_back(port.contract.dtype==DType::Float64?NumericUnit::Seconds:NumericUnit::Values);policy.meanings.push_back(std::move(rule));
    }
}
void essentia_spectral_inputs(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    for(const auto& port:node.inputs){
        metadata_clock(policy,domain::PortDirection::Input,port.id,NumericOperationClockKind::SampleRate,44100);
        auto axis=metadata_uniform(port.id,1,NumericUnit::Hertz,44100./2048);
        axis.allow_index=true;policy.axes.push_back(axis);
        if(port.id)metadata_match_input(policy,port.id,2,true);
    }
}
}
void essentia_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    EssentiaPlan plan;if(!essentia_operation_plan(node,plan))return;
    metadata_index_outputs(node,policy,false);essentia_meanings(node,policy);
    if(plan.kind==EssentiaKind::Onset && plan.method==4){
        metadata_clock(policy,domain::PortDirection::Input,0,NumericOperationClockKind::SampleRate,44100);return;
    }
    if(plan.kind!=EssentiaKind::Agreement && plan.kind!=EssentiaKind::CandidateInputs)essentia_spectral_inputs(node,policy);
}
}
