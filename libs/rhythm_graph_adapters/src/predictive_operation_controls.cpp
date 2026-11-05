// SPDX-License-Identifier: AGPL-3.0-or-later
#include "predictive_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
template<class T> bool changed(const std::optional<T>& value,const std::optional<T>& previous) noexcept {
    return value && value!=previous;
}
bool valid_tempo(float value,const PredictivePlan& plan) noexcept {
    if(!std::isfinite(value) || value<=0)return false;
    if(plan.kind==PredictiveKind::Comb)return true;
    while(value>160)value*=.5f;
    while(value<80)value*=2.f;
    const float period=std::round(60.f*44100.f/(float(plan.config.hop_samples)*value));
    return std::isfinite(period) && period>=1 && period<=float(plan.history);
}
bool valid_period(float value,const PredictivePlan& plan,bool indicate) noexcept {
    if(!std::isfinite(value) || value<1)return false;
    if(indicate)return value<=float(plan.history);
    const float width=std::round(2.f*value);
    return std::isfinite(width) && width<float(SIZE_MAX) && std::size_t(width)<=plan.history;
}
Status read_float(const NumericOperationInput& input,const PredictivePlan& plan,PredictiveWarmControls& controls) noexcept {
    float value=0;const auto status=load_element(input.value,{},value);if(status!=Status::ok)return status;
    if(input.port==11){if(!valid_tempo(value,plan))return Status::invalid_argument;controls.tempo=value;}
    else{
        if(!valid_period(value,plan,input.port==14))return Status::invalid_argument;
        if(input.port==13)controls.period=value;else controls.indicated_period=value;
    }
    return Status::ok;
}
Status read_value(const NumericOperationInput& input,const PredictivePlan& plan,PredictiveWarmControls& controls) noexcept {
    if(input.port==10){
        std::uint32_t value=0;const auto status=load_element(input.value,{},value);if(status!=Status::ok)return status;
        if(value>9)return Status::invalid_argument;controls.method=value;return Status::ok;
    }
    if(input.port==12){
        std::uint8_t value=0;const auto status=load_element(input.value,{},value);if(status!=Status::ok)return status;
        if(value>1)return Status::invalid_argument;controls.fixed=value!=0;return Status::ok;
    }
    return read_float(input,plan,controls);
}
Status validate_control(const NumericOperationInput& input,const domain::ResolvedProcessingNode& node,
    const PredictivePlan& plan,const FrameLimits& limits,PredictiveWarmControls& controls) noexcept {
    NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    if(input.status!=Status::ok || !input.stamp.available)return Status::ok;
    if(input.port==predictive_control_port)return validate_operation_control(input,expected,limits);
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view({input.stamp,input.value},limits);
    return status==Status::ok?read_value(input,plan,controls):status;
}
template<class T> bool tempo_controls(T& source,const PredictiveWarmControls& controls,const PredictiveWarmControls& applied) noexcept {
    const bool tempo_changed=changed(controls.tempo,applied.tempo);
    if(tempo_changed && !source.set_tempo(*controls.tempo))return false;
    if(!controls.fixed || (!changed(controls.fixed,applied.fixed) && !(*controls.fixed && tempo_changed)))return true;
    if(!*controls.fixed){source.release_tempo();return true;}
    return source.fix_tempo(controls.tempo.value_or(source.bpm()));
}
}
Status PredictiveOperation::read_controls(std::span<const NumericOperationInput> inputs,PredictiveWarmControls& controls) const noexcept {
    const auto node=definition_view(descriptor_->node);
    for(const auto& input:inputs){
        if(!input.port)continue;
        const auto status=validate_control(input,node,plan_,limits_,controls);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
bool PredictiveOperation::apply_tempo_controls(const PredictiveWarmControls& controls) noexcept {
    if(pcm_)return tempo_controls(*pcm_,controls,applied_);
    if(auto* source=std::get_if<PreparedPredictiveBeatTracker>(&kernel_))return tempo_controls(*source,controls,applied_);
    if(auto* source=std::get_if<PreparedCombTempoTransition>(&kernel_))return tempo_controls(*source,controls,applied_);
    return true;
}
bool PredictiveOperation::apply_prediction_controls(const PredictiveWarmControls& controls) noexcept {
    auto* source=std::get_if<PreparedBeatPrediction>(&kernel_);if(!source)return true;
    return (!changed(controls.period,applied_.period) || source->set_period(*controls.period)) &&
        (!changed(controls.indicated_period,applied_.indicated_period) || source->indicate_tempo(*controls.indicated_period));
}
bool PredictiveOperation::apply_controls(const PredictiveWarmControls& controls) noexcept {
    if(changed(controls.method,applied_.method)){
        const auto method=PredictiveOnsetMethod(*controls.method);
        if(pcm_){if(!pcm_->set_method(method))return false;}
        else if(!std::get<PreparedPcmPredictiveOnset>(kernel_).set_method(method))return false;
    }
    if(!apply_tempo_controls(controls) || !apply_prediction_controls(controls))return false;
    if(controls.method)applied_.method=controls.method;
    if(controls.tempo)applied_.tempo=controls.tempo;
    if(controls.fixed)applied_.fixed=controls.fixed;
    if(controls.period)applied_.period=controls.period;
    if(controls.indicated_period)applied_.indicated_period=controls.indicated_period;
    return true;
}
}
