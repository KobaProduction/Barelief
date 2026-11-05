// SPDX-License-Identifier: AGPL-3.0-or-later
#include "predictive_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool PredictiveOperation::resample(void* context,std::span<const float> input,std::span<float> output) noexcept {
    std::size_t written=0;return static_cast<dsp_kernels::PreparedResampler*>(context)->process_window(input,input.size(),output,output.size(),written) && written==output.size();
}
bool PredictiveOperation::prepare_kernel() {
    switch(plan_.kind){
    case PredictiveKind::Frontend:return kernel_.emplace<PreparedPcmPredictiveOnset>().prepare(plan_.config);
    case PredictiveKind::Comb:return kernel_.emplace<PreparedCombTempoTransition>().prepare(plan_.config.hop_samples);
    case PredictiveKind::Prediction:return kernel_.emplace<PreparedBeatPrediction>().prepare(plan_.history,plan_.period,plan_.tightness,plan_.alpha);
    case PredictiveKind::PcmTracker:
        pcm_=std::make_unique<PreparedPcmPredictiveBeatTracker>();return pcm_->prepare(*provider_,plan_.config);
    case PredictiveKind::Tracker:
        window_=std::make_unique<dsp_kernels::PreparedResampler>();
        return window_->prepare_window({plan_.history,512,1},provider_.get()) &&
            kernel_.emplace<PreparedPredictiveBeatTracker>().prepare(plan_.config.hop_samples,{window_.get(),resample});
    }
    return false;
}
bool PredictiveOperation::prepare(const domain::ConfiguredProcessingNode& node,std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> provider) {
    if(!predictive_operation_plan(definition_view(node),plan_))return false;
    if(plan_.kind==PredictiveKind::Tracker || plan_.kind==PredictiveKind::PcmTracker)provider_=std::move(provider);
    return predictive_operation_storage(plan_,provider_.get()) && prepare_kernel() && workspace_bytes()<=plan_.kernel_bytes;
}
std::unique_ptr<AdapterOperation> prepare_predictive_operation(const domain::ConfiguredProcessingNode& node,
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> provider) {
    auto result=std::make_unique<PredictiveOperation>();if(!result->prepare(node,std::move(provider)))return {};return result;
}
std::size_t PredictiveOperation::workspace_bytes() const noexcept {
    const auto bytes=std::visit([](const auto& source)->std::size_t {
        if constexpr(std::is_same_v<std::decay_t<decltype(source)>,std::monostate>)return 0;
        else return source.workspace_bytes();
    },kernel_);
    return bytes+(pcm_?sizeof(*pcm_)+pcm_->workspace_bytes():0)+(window_?window_->resources().owned_bytes:0);
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> PredictiveOperation::clone() const {
    auto result=std::make_unique<PredictiveOperation>();result->bind(descriptor_,admission_);
    result->plan_=plan_;result->provider_=provider_;result->kernel_=kernel_;result->prediction_=prediction_;result->pcm_result_=pcm_result_;result->value_=value_;result->applied_=applied_;
    if(pcm_){result->pcm_=pcm_->clone();if(!result->pcm_)return {};}
    if(window_){
        result->window_=std::make_unique<dsp_kernels::PreparedResampler>(*window_);
        if(!result->window_->window_config() || !result->window_->copy_state_from(*window_) ||
           !std::get<PreparedPredictiveBeatTracker>(result->kernel_).set_resampling_port({result->window_.get(),resample}))return {};
    }
    return result;
}
void PredictiveOperation::reset() noexcept {
    if(pcm_)pcm_->restart();
    std::visit([&](auto& source){
        using T=std::decay_t<decltype(source)>;
        if constexpr(std::is_same_v<T,PreparedPcmPredictiveOnset>)source.restart();
        else if constexpr(std::is_same_v<T,PreparedBeatPrediction>)source.reset(plan_.period);
        else if constexpr(!std::is_same_v<T,std::monostate>)source.reset();
    },kernel_);
    if(window_)window_->reset();prediction_={};pcm_result_={};value_=0;applied_={};
}
signal_contracts::Status PredictiveOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept {
    reset();return signal_contracts::Status::incompatible_contract;
}
}
