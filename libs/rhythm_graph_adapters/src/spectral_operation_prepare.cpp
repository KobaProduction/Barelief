// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
std::unique_ptr<AdapterOperation> prepare_spectral_operation(const domain::ConfiguredProcessingNode& node) {
    if(!spectral_operation_kind(node.kind))return {};
    auto operation=std::make_unique<SpectralOperation>();if(!operation->prepare(node))return {};
    return operation;
}
bool SpectralOperation::prepare_kernel() {
    switch(plan_.kind){
    case SpectralOperationKind::Polar:return kernel_.emplace<PreparedPolarNovelty>().prepare(plan_.bins,plan_.polar);
    case SpectralOperationKind::Histogram:return kernel_.emplace<PreparedHistogramOnset>().prepare(plan_.bins,plan_.histogram);
    case SpectralOperationKind::Whitening:return kernel_.emplace<PreparedSpectralWhitening>().prepare(plan_.bins,plan_.magnitude.whitening);
    case SpectralOperationKind::Magnitude:return kernel_.emplace<PreparedOnsetMagnitude>().prepare(plan_.bins,plan_.magnitude);
    case SpectralOperationKind::Rms:kernel_.emplace<PreparedSpectralRmsFlux>();return true;
    }
    return false;
}
bool SpectralOperation::prepare(const domain::ConfiguredProcessingNode& node) {
    const auto view=definition_view(node);
    if(!plan_spectral_operation(view,plan_) || !spectral_output_contracts(view,plan_) || !prepare_kernel())return false;
    if(plan_.output_buffer_bytes)values_.resize(plan_.bins);
    std::size_t bound=plan_.kernel_bytes;
    return dataflow_runtime::numeric_operation_add_bytes(bound,plan_.output_buffer_bytes) && workspace_bytes()<=bound;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> SpectralOperation::clone() const {return std::make_unique<SpectralOperation>(*this);}
void SpectralOperation::reset() noexcept {
    std::visit([](auto& kernel){
        using T=std::decay_t<decltype(kernel)>;
        if constexpr(std::is_same_v<T,PreparedOnsetMagnitude>)kernel.reset_whitening();
        else kernel.reset();
    },kernel_);
    result_=0;
}
signal_contracts::Status SpectralOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation& source) noexcept {
    const auto* other=dynamic_cast<const SpectralOperation*>(&source);
    if(other && plan_.kind==SpectralOperationKind::Rms && other->plan_.kind==plan_.kind && compatible(*other)){
        std::get<PreparedSpectralRmsFlux>(kernel_)=std::get<PreparedSpectralRmsFlux>(other->kernel_);result_=other->result_;
        return signal_contracts::Status::ok;
    }
    // Declared reset on reprepare for vector-history source operations. A cold
    // clone still deep-copies their complete current history independently.
    reset();return signal_contracts::Status::incompatible_contract;
}
std::size_t SpectralOperation::workspace_bytes() const noexcept {
    const auto bytes=std::visit([](const auto& kernel)->std::size_t {
        using T=std::decay_t<decltype(kernel)>;
        if constexpr(std::is_same_v<T,PreparedSpectralRmsFlux>)return 0;
        else return kernel.workspace_bytes();
    },kernel_);
    return bytes+values_.capacity()*sizeof(float);
}
}
