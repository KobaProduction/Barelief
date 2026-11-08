// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_buffers.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
bool PcmStageOperation::calculate(std::span<const NumericOperationInput> inputs,const PcmStageControls& controls) noexcept {
    offsets_count_=0;tatum_=0;
    if(plan_.kind==PcmStageKind::Tracker)return std::get<PreparedContextBeatTracker>(kernel_).process(
        input_at(inputs,context_tracker_ports::reversed_weighted_history)->contiguous_float32,
        input_at(inputs,context_tracker_ports::autocorrelation_input)->contiguous_float32,offsets_,offsets_count_);
    return calculate_sequence(inputs,controls);
}
bool PcmStageOperation::calculate_sequence(std::span<const NumericOperationInput> inputs,const PcmStageControls& controls) noexcept {
    std::uint8_t silent=0;
    if(load_element(input_at(inputs,context_sequence_ports::silence)->value,{0,0,0},silent)!=Status::ok)return false;
    auto& source=std::get<PreparedContextBeatSequence>(kernel_);
    if(!source.process(input_at(inputs,context_sequence_ports::reversed_weighted_history)->contiguous_float32,
        input_at(inputs,context_sequence_ports::autocorrelation_input)->contiguous_float32,silent!=0,beat_))return false;
    if(controls.poll_tatum)tatum_=source.poll_tatum();return true;
}
Status PcmStageOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    const auto stamp=operation_frame_stamp(input_at(inputs,0),false);
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !pcm_stage_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,stamp);
    }
    return process_enabled(inputs,outputs,control,stamp);
}
Status PcmStageOperation::process_enabled(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control,const NumericOperationOrigin& stamp) noexcept {
    PcmStageControls controls;bool has_frame=false;
    auto status=validate_call(inputs,outputs,controls,has_frame,control.event);if(status!=Status::ok || !has_frame)return status;
    if(!apply_controls(controls) || !calculate(inputs,controls))return Status::invalid_argument;
    // Outputs represent a window/frame stage, not the incoming hop's interval.
    status=publish(inputs,outputs,stamp);if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
Status PcmStageOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) const noexcept {
    auto status=prepare_output_views(outputs);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.origin=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
}
