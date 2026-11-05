// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool DescriptorOperation::prepare_kernel(const domain::ResolvedProcessingNode& node) {
    switch(plan_.kind){
    case DescriptorKind::Onset:return kernel_.emplace<PreparedOnsetDescriptor>().prepare(plan_.bins,plan_.recipe);
    case DescriptorKind::Spectral:return kernel_.emplace<PreparedSpectralOnset>().prepare(plan_.bins,plan_.spectral);
    case DescriptorKind::Bands:return kernel_.emplace<PreparedTriangularSpectralBands>().prepare(plan_.bins,array_parameter(node,"edges_hz"),plan_.triangular);
    }
    return false;
}
bool DescriptorOperation::prepare(const domain::ConfiguredProcessingNode& node) {
    const auto view=definition_view(node);
    if(!plan_descriptor_operation(view,plan_) || !descriptor_output_contracts(view,plan_) || !prepare_kernel(view))return false;
    if(plan_.buffer_bytes)values_.resize(plan_.bands);
    std::size_t bound=plan_.kernel_bytes;
    return dataflow_runtime::numeric_operation_add_bytes(bound,plan_.buffer_bytes) && workspace_bytes()<=bound;
}
std::unique_ptr<AdapterOperation> prepare_descriptor_operation(const domain::ConfiguredProcessingNode& node) {
    if(!descriptor_operation_kind(node.kind))return {};
    auto operation=std::make_unique<DescriptorOperation>();if(!operation->prepare(node))return {};
    return operation;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> DescriptorOperation::clone() const {
    return std::make_unique<DescriptorOperation>(*this);
}
void DescriptorOperation::reset() noexcept {
    if(plan_.kind==DescriptorKind::Onset)std::get<PreparedOnsetDescriptor>(kernel_).restart();
    else if(plan_.kind==DescriptorKind::Spectral)std::get<PreparedSpectralOnset>(kernel_).reset();
    result_=0;
}
signal_contracts::Status DescriptorOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation& source) noexcept {
    const auto* other=dynamic_cast<const DescriptorOperation*>(&source);
    if(other && plan_.kind==DescriptorKind::Bands && compatible(*other))return signal_contracts::Status::ok;
    // Source histories survive calls within this instance and cold clone.
    // Reprepare explicitly resets them rather than allocating on state copy.
    reset();return signal_contracts::Status::incompatible_contract;
}
std::size_t DescriptorOperation::workspace_bytes() const noexcept {
    return std::visit([](const auto& kernel){return kernel.workspace_bytes();},kernel_)+values_.capacity()*sizeof(float);
}
}
