// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status context_rows(NumericOperationOutput& output,domain::PortId port,const PreparedContextBeatSequence& source,const FrameStamp& stamp) noexcept {
    if(port==9)return write_vector(output,source.onset_window(),stamp);
    if(port==10)return write_vector(output,source.tracker().autocorrelation(),stamp);
    if(port==11)return write_vector(output,source.tracker().comb_response(),stamp);
    if(port==12)return write_vector(output,source.tracker().phase_response(),stamp);
    if(port==13)return write_vector(output,source.scheduled_offsets(),stamp);
    return Status::invalid_argument;
}
}
Status PcmStageOperation::publish_context(NumericOperationOutput& output,domain::PortId port,
    const PreparedContextBeatSequence& source,const FrameStamp& stamp) noexcept {
    const auto& frame=plan_.kind==PcmStageKind::Tempo?tempo_.beat:beat_;
    if(port==0)return write_pcm_scalar(output,std::uint8_t(frame.scheduled),stamp);
    if(port==1)return write_pcm_scalar(output,std::uint8_t(frame.silent),stamp);
    if(port==6)return write_pcm_scalar(output,frame.beat_sample,stamp);
    if(port==7)return write_pcm_scalar(output,source.last_tatum_sample(),stamp);
    if(port==8)return write_pcm_scalar(output,std::uint32_t(tatum_),stamp);
    if(port<6){
        const std::array<float,4> values{frame.source_tactus,frame.period_samples,frame.bpm,frame.confidence};
        return write_pcm_scalar(output,values[port-2],stamp);
    }
    return context_rows(output,port,source,stamp);
}
Status PcmStageOperation::publish_tracker(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    const auto& source=std::get<PreparedContextBeatTracker>(kernel_);const auto port=output.port;
    if(port==0)return write_vector(output,std::span<const float>(offsets_).first(offsets_count_),stamp);
    if(port==1)return write_pcm_scalar(output,source.period_frames(),stamp);
    if(port==2)return write_pcm_scalar(output,source.bpm(),stamp);
    if(port==3)return write_pcm_scalar(output,source.confidence(),stamp);
    if(port==4)return write_pcm_scalar(output,std::uint32_t(source.time_signature()),stamp);
    if(port==5)return write_vector(output,source.autocorrelation(),stamp);
    if(port==6)return write_vector(output,source.comb_response(),stamp);
    if(port==7)return write_vector(output,source.phase_response(),stamp);
    return Status::invalid_argument;
}
}
