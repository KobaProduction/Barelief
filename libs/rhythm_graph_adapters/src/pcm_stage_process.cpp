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
    if(plan_.kind==PcmStageKind::Tracker)return std::get<PreparedContextBeatTracker>(kernel_).process(input_at(inputs,0)->contiguous_float32,offsets_,offsets_count_);
    if(plan_.kind==PcmStageKind::Sequence)return calculate_sequence(inputs,controls);
    // Validated complete PCM hops are appended before any spectral arithmetic.
    // This local coordinate counter includes an acquired arithmetic-failure hop.
    samples_+=plan_.onset.spectrum.hop_samples;const auto pcm=input_at(inputs,0)->contiguous_float32;
    if(plan_.kind==PcmStageKind::Spectrum)return std::get<PreparedHopSpectrum>(kernel_).process_hop(pcm,magnitude_,phase_);
    if(plan_.kind==PcmStageKind::Onset)return std::get<PreparedPcmOnsetSequence>(kernel_).process_hop(pcm,onset_);
    return calculate_tempo(pcm,controls);
}
bool PcmStageOperation::calculate_sequence(std::span<const NumericOperationInput> inputs,const PcmStageControls& controls) noexcept {
    float descriptor=0;std::uint8_t silent=0;
    if(load_element(input_at(inputs,0)->value,{0,0,0},descriptor)!=Status::ok ||
       load_element(input_at(inputs,1)->value,{0,0,0},silent)!=Status::ok)return false;
    auto& source=std::get<PreparedContextBeatSequence>(kernel_);if(!source.process(descriptor,silent!=0,beat_))return false;
    if(controls.poll_tatum)tatum_=source.poll_tatum();return true;
}
bool PcmStageOperation::calculate_tempo(std::span<const float> pcm,const PcmStageControls& controls) noexcept {
    auto& source=std::get<PreparedPcmContextTempo>(kernel_);if(!source.process_hop(pcm,tempo_))return false;
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
    PcmStageControls controls;bool has_frame=false;
    auto status=validate_call(inputs,outputs,controls,has_frame,control.event);if(status!=Status::ok || !has_frame)return status;
    if(!apply_controls(controls) || !calculate(inputs,controls))return Status::invalid_argument;
    // Outputs represent a window/frame stage, not the incoming hop's interval.
    status=publish(outputs,stamp);if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
Status PcmStageOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) const noexcept {
    auto status=prepare_output_views(outputs,false);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.stamp=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
}
