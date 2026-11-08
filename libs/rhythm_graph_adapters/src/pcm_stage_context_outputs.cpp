// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status context_rows(NumericOperationOutput& output,domain::PortId port,const PreparedContextBeatSequence& source,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    if(port==context_sequence_ports::autocorrelation)return write_vector(output,source.tracker().autocorrelation(),stamp);
    if(port==context_sequence_ports::comb_response)return write_vector(output,source.tracker().comb_response(),stamp);
    if(port==context_sequence_ports::phase_response)return write_vector(output,source.tracker().phase_response(),stamp);
    if(port==context_sequence_ports::offsets)return write_vector(output,source.scheduled_offsets(),stamp);
    return Status::invalid_argument;
}
Status context_status(NumericOperationOutput& output,domain::PortId port,const ContextBeatFrame& frame,
    const PreparedContextBeatSequence& source,unsigned tatum,const NumericOperationOrigin& stamp) noexcept {
    if(port==context_sequence_ports::scheduled)return write_pcm_scalar(output,std::uint8_t(frame.scheduled),stamp);
    if(port==context_sequence_ports::silent)return write_pcm_scalar(output,std::uint8_t(frame.silent),stamp);
    if(port==context_sequence_ports::beat_sample)return write_pcm_scalar(output,frame.beat_sample,stamp);
    if(port==context_sequence_ports::tatum_sample)return write_pcm_scalar(output,source.last_tatum_sample(),stamp);
    if(port==context_sequence_ports::tatum_event)return write_pcm_scalar(output,std::uint32_t(tatum),stamp);
    return Status::invalid_argument;
}
Status tracker_scalar(NumericOperationOutput& output,const PreparedContextBeatTracker& source,const NumericOperationOrigin& stamp) noexcept {
    if(output.port==context_tracker_ports::period_frames)return write_pcm_scalar(output,source.period_frames(),stamp);
    if(output.port==context_tracker_ports::bpm)return write_pcm_scalar(output,source.bpm(),stamp);
    if(output.port==context_tracker_ports::confidence)return write_pcm_scalar(output,source.confidence(),stamp);
    if(output.port==context_tracker_ports::meter)return write_pcm_scalar(output,std::uint32_t(source.time_signature()),stamp);
    return Status::invalid_argument;
}
Status tracker_rows(NumericOperationOutput& output,const PreparedContextBeatTracker& source,const NumericOperationOrigin& stamp) noexcept {
    if(output.port==context_tracker_ports::autocorrelation)return write_vector(output,source.autocorrelation(),stamp);
    if(output.port==context_tracker_ports::comb_response)return write_vector(output,source.comb_response(),stamp);
    if(output.port==context_tracker_ports::phase_response)return write_vector(output,source.phase_response(),stamp);
    return Status::invalid_argument;
}

}
Status PcmStageOperation::publish_context(NumericOperationOutput& output,domain::PortId port,
    const PreparedContextBeatSequence& source,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    const auto& frame=beat_;
    if(port>=context_sequence_ports::tactus && port<context_sequence_ports::beat_sample){
        const std::array<float,4> values{frame.source_tactus,frame.period_samples,frame.bpm,frame.confidence};
        return write_pcm_scalar(output,values[port-context_sequence_ports::tactus],stamp);
    }
    if(port<context_sequence_ports::onset_history)return context_status(output,port,frame,source,tatum_,stamp);
    return context_rows(output,port,source,stamp);
}
Status PcmStageOperation::publish_tracker(NumericOperationOutput& output,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    const auto& source=std::get<PreparedContextBeatTracker>(kernel_);const auto port=output.port;
    if(port==context_tracker_ports::offsets)return write_vector(output,std::span<const float>(offsets_).first(offsets_count_),stamp);
    return port<context_tracker_ports::autocorrelation?tracker_scalar(output,source,stamp):tracker_rows(output,source,stamp);
}
}
