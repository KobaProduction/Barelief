// SPDX-License-Identifier: AGPL-3.0-or-later
#include "event_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
template<class T>
Status write_event_scalar(NumericOperationOutput& output,T value,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={1,1};const auto status=store_element(output.value,{0,0,0},value);
    if(status==Status::ok){output.origin=stamp;output.status=Status::ok;}return status;
}
Status write_decision(std::span<NumericOperationOutput> outputs,const OnsetDecision& decision,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    auto status=write_event_scalar(*output_at(outputs,onset_decision_ports::accepted_position),decision.peak_position,stamp);if(status!=Status::ok)return status;
    status=write_event_scalar(*output_at(outputs,onset_decision_ports::last_sample),decision.sample,stamp);if(status!=Status::ok)return status;
    return write_event_scalar(*output_at(outputs,onset_decision_ports::detected),std::uint8_t(decision.detected),stamp);
}

}
bool EventOperation::calculate(std::span<const NumericOperationInput> inputs,float peak,bool silent) noexcept {
    switch(plan_.kind){
    case EventOperationKind::Picker:return std::get<PreparedAdaptivePeakPicker>(kernel_).process(input_at(inputs,peak_picker_ports::smoothed_history)->contiguous_float32,peak_);
    case EventOperationKind::Decision:return std::get<PreparedOnsetDecision>(kernel_).process(peak,silent,decision_);
    case EventOperationKind::Silence:return pcm_is_silent(input_at(inputs,0)->contiguous_float32,plan_.silence_db,silent_);
    }
    return false;
}
Status EventOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,operation_frame_stamp(input_at(inputs,0),false));
    }
    return process_enabled(inputs,outputs);
}
Status EventOperation::process_enabled(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept {
    float peak=0;bool silent=false;
    auto status=validate_call(inputs,outputs,peak,silent);if(status!=Status::ok)return status;
    if(!calculate(inputs,peak,silent))return Status::invalid_argument;
    status=publish(outputs,operation_point_stamp(input_at(inputs,0)));
    if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
Status EventOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    auto status=validate_event_outputs(outputs);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        output.value.actual={1,1};status=fill_silence(output.value,limits_);
        if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.origin=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
Status EventOperation::publish(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    if(plan_.kind==EventOperationKind::Silence)return write_event_scalar(*output_at(outputs,silence_ports::silent),std::uint8_t(silent_),stamp);
    if(plan_.kind==EventOperationKind::Decision)return write_decision(outputs,decision_,stamp);
    const std::array<float,5> values{peak_.position,0,peak_.thresholded,peak_.mean,peak_.median};
    for(auto& output:outputs){
        const auto status=output.port==peak_picker_ports::detected?write_event_scalar(output,std::uint8_t(peak_.peak),stamp):write_event_scalar(output,values[output.port],stamp);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
