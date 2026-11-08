// SPDX-License-Identifier: AGPL-3.0-or-later
#include "btrack_operations.h"
#include "parameter_values.h"
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool BTrackDecision::prepare() {return tempo.prepare(512) && prediction.prepare(512,43,5,.9f);}
void BTrackDecision::reset() noexcept {tempo.reset();prediction.reset(43);result={};}
bool BTrackDecision::process(float onset,std::span<const float> observations) noexcept {
    BeatPredictionObservation next;
    if(!prediction.process(onset,next))return false;
    if(next.beat_due){
        float period=0;if(!tempo.advance_observations(observations,period) || !prediction.set_period(period))return false;
        next.period_frames=period;
    }
    result=next;return true;
}
bool BTrackOperation::prepare_onset() {return kernel_.emplace<BTrackSourceOnset>().prepare();}
bool BTrackOperation::prepare_history() {
    if(!kernel_.emplace<ScalarHistory>().prepare(512))return false;
    reset();return true;
}
bool BTrackOperation::prepare_kernel() {
    if(plan_.kind==BTrackKind::Threshold || plan_.kind==BTrackKind::Comb)return true;
    if(plan_.kind==BTrackKind::Onset)return prepare_onset();
    if(plan_.kind==BTrackKind::History)return prepare_history();
    if(plan_.kind==BTrackKind::Response)return kernel_.emplace<PreparedBTrackCombResponse>().prepare();
    if(plan_.kind==BTrackKind::Decision)return kernel_.emplace<BTrackDecision>().prepare();
    resampler_=std::make_unique<dsp_kernels::PreparedResampler>();return resampler_->prepare_window(btrack_sinc_config(),provider_.get());
}
bool BTrackOperation::prepare(const dataflow_runtime::NumericOperationNode& node,std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> provider) {
    if(!btrack_plan(definition_view(node),plan_))return false;
    if(plan_.kind==BTrackKind::Resample)provider_=std::move(provider);
    return btrack_storage(plan_,provider_.get()) && prepare_kernel() && workspace_bytes()<=plan_.bytes;
}
std::unique_ptr<AdapterOperation> prepare_btrack_operation(const dataflow_runtime::NumericOperationNode& node,
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> provider) {
    auto result=std::make_unique<BTrackOperation>();if(!result->prepare(node,std::move(provider)))return {};return result;
}
std::size_t BTrackOperation::workspace_bytes() const noexcept {
    const auto bytes=std::visit([](const auto& kernel)->std::size_t {
        if constexpr(std::is_same_v<std::decay_t<decltype(kernel)>,std::monostate>)return 0;
        else return kernel.workspace_bytes();
    },kernel_);
    return bytes+(resampler_?resampler_->resources().owned_bytes:0);
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> BTrackOperation::clone() const {
    auto result=std::make_unique<BTrackOperation>();result->bind(descriptor_,admission_);
    result->plan_=plan_;result->provider_=provider_;result->kernel_=kernel_;result->resampled_=resampled_;result->scalar_=scalar_;
    if(resampler_){
        result->resampler_=std::make_unique<dsp_kernels::PreparedResampler>(*resampler_);
        if(!result->resampler_->copy_state_from(*resampler_))return {};
    }
    return result;
}
void BTrackOperation::reset() noexcept {
    std::visit([](auto& kernel){
        using T=std::decay_t<decltype(kernel)>;
        if constexpr(std::is_same_v<T,ScalarHistory>){kernel.clear();for(std::size_t i=0;i<512;++i)kernel.set(i,i%43==0?1.f:0.f);}
        else if constexpr(!std::is_same_v<T,std::monostate>)kernel.reset();
    },kernel_);
    if(resampler_)resampler_->reset();resampled_.fill(0);scalar_=0;
}
signal_contracts::Status BTrackOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept {
    // A clone is independent and complete; Apply declares reset rather than
    // allocating for histories/backends during compatible-state migration.
    reset();return signal_contracts::Status::incompatible_contract;
}
}
