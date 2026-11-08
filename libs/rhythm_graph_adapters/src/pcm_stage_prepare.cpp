// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool PcmStageOperation::prepare_kernel() {
    switch(plan_.kind){
    case PcmStageKind::Tracker:return kernel_.emplace<PreparedContextBeatTracker>().prepare(plan_.tracker);
    case PcmStageKind::Sequence:return kernel_.emplace<PreparedContextBeatSequence>().prepare(plan_.sequence);
    }
    return false;
}
bool PcmStageOperation::prepare(const dataflow_runtime::NumericOperationNode& node) {
    const auto view=definition_view(node);
    if(!plan_pcm_stage(view,plan_) || !pcm_stage_output_contracts(view,plan_) || !prepare_kernel())return false;
    if(plan_.kind==PcmStageKind::Tracker)offsets_.resize(plan_.history/4+1);
    std::size_t bytes=plan_.kernel_bytes;
    return dataflow_runtime::numeric_operation_add_bytes(bytes,plan_.buffer_bytes) && workspace_bytes()<=bytes;
}
std::unique_ptr<AdapterOperation> prepare_pcm_stage(const dataflow_runtime::NumericOperationNode& node) {
    if(!pcm_stage_operation_kind(node.kind))return {};
    auto operation=std::make_unique<PcmStageOperation>();if(!operation->prepare(node))return {};
    return operation;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> PcmStageOperation::clone() const {return std::make_unique<PcmStageOperation>(*this);}
std::size_t PcmStageOperation::workspace_bytes() const noexcept {
    return std::visit([](const auto& kernel){return kernel.workspace_bytes();},kernel_)+
        offsets_.capacity()*sizeof(float);
}
void PcmStageOperation::reset() noexcept {
    std::visit([](auto& kernel){kernel.reset();},kernel_);
    offsets_count_=0;tatum_=0;beat_={};
}
signal_contracts::Status PcmStageOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept {
    reset();return signal_contracts::Status::incompatible_contract;
}
}
