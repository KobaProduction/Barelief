// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool ContrastOperation::prepare(const domain::ConfiguredProcessingNode& node) {
    const auto view=definition_view(node);
    if(!plan_contrast_operation(view,plan_) || !contrast_output_contracts(view,plan_))return false;
    const bool ready=plan_.accumulator?kernel_.emplace<PreparedContrastPeakAccumulator>().prepare(plan_.maximum,plan_.events,plan_.config):
        kernel_.emplace<PreparedContrastPeakPicker>().prepare(plan_.maximum,plan_.config);
    if(!ready)return false;
    if(plan_.accumulator)final_events_.resize(plan_.events);
    std::size_t bytes=plan_.kernel_bytes;
    return dataflow_runtime::numeric_operation_add_bytes(bytes,plan_.buffer_bytes) && workspace_bytes()<=bytes;
}
std::unique_ptr<AdapterOperation> prepare_contrast_operation(const domain::ConfiguredProcessingNode& node) {
    if(!contrast_operation_kind(node.kind))return {};
    auto operation=std::make_unique<ContrastOperation>();if(!operation->prepare(node))return {};
    return operation;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> ContrastOperation::clone() const {return std::make_unique<ContrastOperation>(*this);}
std::size_t ContrastOperation::workspace_bytes() const noexcept {
    return std::visit([](const auto& kernel){return kernel.workspace_bytes();},kernel_)+final_events_.capacity()*sizeof(float);
}
void ContrastOperation::reset() noexcept {
    std::visit([](auto& kernel){kernel.reset();},kernel_);count_=0;rows_=0;source_stamp_={};
}
signal_contracts::Status ContrastOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept {
    reset();return signal_contracts::Status::incompatible_contract;
}
}
