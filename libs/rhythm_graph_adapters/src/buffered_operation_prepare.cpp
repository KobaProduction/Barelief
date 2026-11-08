// SPDX-License-Identifier: AGPL-3.0-or-later
#include "buffered_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool BufferedOperation::prepare(const dataflow_runtime::NumericOperationNode& node) {
    const auto view=definition_view(node);
    if(!plan_buffered_operation(view,plan_) || !buffered_output_contracts(view,plan_))return false;
    bool ready=false;
    if(plan_.kind==BufferedOperationKind::Maximum)ready=kernel_.emplace<PreparedBufferedMaximum>().prepare(plan_.width,plan_.causal);
    else if(plan_.kind==BufferedOperationKind::Average)ready=kernel_.emplace<PreparedCausalAverage>().prepare(plan_.width);
    else ready=kernel_.emplace<PreparedBufferedBandFlux>().prepare(plan_.maximum,plan_.maximum_frames,plan_.flux);
    if(!ready)return false;
    if(plan_.buffer_bytes)values_.resize(plan_.maximum);
    std::size_t bound=plan_.kernel_bytes;
    return dataflow_runtime::numeric_operation_add_bytes(bound,plan_.buffer_bytes) && workspace_bytes()<=bound;
}
std::unique_ptr<AdapterOperation> prepare_buffered_operation(const dataflow_runtime::NumericOperationNode& node) {
    if(!buffered_operation_kind(node.kind))return {};
    auto operation=std::make_unique<BufferedOperation>();if(!operation->prepare(node))return {};
    return operation;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> BufferedOperation::clone() const {return std::make_unique<BufferedOperation>(*this);}
std::size_t BufferedOperation::workspace_bytes() const noexcept {
    return std::visit([](const auto& kernel){return kernel.workspace_bytes();},kernel_)+values_.capacity()*sizeof(float);
}
void BufferedOperation::reset() noexcept {
    std::visit([](auto& kernel){kernel.reset();},kernel_);result_=0;
}
signal_contracts::Status BufferedOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept {
    reset();return signal_contracts::Status::incompatible_contract;
}
}
