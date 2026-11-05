// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
Status PcmStageOperation::publish_onset(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    const auto& source=std::get<PreparedPcmOnsetSequence>(kernel_);const auto port=output.port;
    if(port<5){
        const std::array<float,5> values{onset_.descriptor,onset_.peak.position,onset_.peak.thresholded,onset_.peak.mean,onset_.peak.median};
        return write_pcm_scalar(output,values[port],stamp);
    }
    if(port==5)return write_pcm_scalar(output,std::uint8_t(onset_.onset.detected),stamp);
    if(port==6)return write_pcm_scalar(output,onset_.onset.sample,stamp);
    if(port==7)return write_pcm_scalar(output,std::uint8_t(onset_.silent),stamp);
    if(port==10)return write_vector(output,source.whitening_peaks(),stamp);
    return publish_spectrum(output,port<10?domain::PortId(port-8):domain::PortId(port-9),source.spectrum_stage(),source.processed_magnitude(),source.phase(),stamp);
}
Status PcmStageOperation::publish_tempo(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    const auto& source=std::get<PreparedPcmContextTempo>(kernel_);const auto port=output.port;
    if(port<3){
        const std::array<float,3> values{tempo_.descriptor,tempo_.onset_peak.position,tempo_.onset_peak.thresholded};
        return write_pcm_scalar(output,values[port],stamp);
    }
    if(port<11){
        const auto relative=port==3?domain::PortId{0}:domain::PortId(port-2);
        return publish_context(output,relative,source.sequence_stage(),stamp);
    }
    if(port==11)return write_vector(output,source.raw_magnitude(),stamp);
    if(port==12)return write_vector(output,source.phase(),stamp);
    if(port<18)return publish_context(output,domain::PortId(port-4),source.sequence_stage(),stamp);
    return publish_spectrum(output,domain::PortId(port-16),source.spectrum_stage(),source.raw_magnitude(),source.phase(),stamp);
}
Status PcmStageOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    for(auto& output:outputs){
        Status status=Status::invalid_argument;
        switch(plan_.kind){
        case PcmStageKind::Spectrum:status=publish_spectrum(output,output.port,std::get<PreparedHopSpectrum>(kernel_),magnitude_,phase_,stamp);break;
        case PcmStageKind::Onset:status=publish_onset(output,stamp);break;
        case PcmStageKind::Tempo:status=publish_tempo(output,stamp);break;
        case PcmStageKind::Tracker:status=publish_tracker(output,stamp);break;
        case PcmStageKind::Sequence:status=publish_context(output,output.port,std::get<PreparedContextBeatSequence>(kernel_),stamp);break;
        }
        if(status!=Status::ok)return status;
        status=validate_values(output.value.view(),pcm_stage_output_spec(plan_,output.port).values,limits_);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
