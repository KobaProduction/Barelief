// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
Status SequenceSourceOperation::publish_onsets(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    const auto port=output.port;
    if(plan_.kind==SequenceSourceKind::Fusion)return write_vector(output,port?std::get<PreparedNormalizedFeatureFusion>(kernel_).maxima():std::span<const float>(values_).first(result_rows_),stamp);
    if(port==0)return sequence_source_flags(output,std::span<const std::uint8_t>(flags_).first(result_rows_),stamp);
    if(port==1)return write_vector(output,std::span<const float>(times_).first(event_count_),stamp);
    if(plan_.kind==SequenceSourceKind::Selection){
        const auto& source=std::get<PreparedAdaptiveOnsetSelection>(kernel_);
        if(port==4)return sequence_source_flags(output,source.decisions(),stamp);
        return write_vector(output,port==2?source.gated_curve():source.thresholds(),stamp);
    }
    const auto& source=std::get<PreparedOnsetEvents>(kernel_);
    if(port==2)return write_vector(output,source.fused_curve(),stamp);
    if(port==3)return write_vector(output,source.smoothed_curve(),stamp);
    if(port==4)return write_vector(output,source.selection().gated_curve(),stamp);
    if(port==5)return write_vector(output,source.selection().thresholds(),stamp);
    return sequence_source_flags(output,source.selection().decisions(),stamp);
}
namespace {
Status histogram_peaks(NumericOperationOutput& output,const BeatIntervalHistogram& histogram,const FrameStamp& stamp) noexcept {
    output.value.actual={2,3};
    for(std::size_t row=0;row<2;++row){
        const auto& peak=histogram.peaks[row];const std::array<float,3> values{peak.bpm,peak.weight,peak.spread};
        for(std::size_t column=0;column<3;++column){const auto status=store_element(output.value,{0,row,column},values[column]);if(status!=Status::ok)return status;}
    }
    output.stamp=stamp;output.status=Status::ok;return Status::ok;
}
}
Status SequenceSourceOperation::publish_summary(NumericOperationOutput& output,const PreparedBeatIntervalSummary& source,const FrameStamp& stamp) noexcept {
    auto port=output.port;const bool pcm=plan_.kind==SequenceSourceKind::PcmSummary;
    if(!port)return write_pcm_scalar(output,source.bpm(),stamp);
    if(port==1)return write_pcm_scalar(output,source.confidence(),stamp);
    if(pcm){if(port==2)return write_vector(output,std::get<PreparedPcmRhythmSummary>(kernel_).tracker().ticks(),stamp);--port;}
    if(port==2)return write_vector(output,source.intervals(),stamp);
    if(port==3)return write_vector(output,source.estimates(),stamp);
    if(port==6)return write_pcm_scalar(output,std::uint64_t(source.intervals().size()),stamp);
    if(!histogram_ready_)return Status::ok;
    return port==4?write_vector(output,histogram_.histogram,stamp):histogram_peaks(output,histogram_,stamp);
}
}
