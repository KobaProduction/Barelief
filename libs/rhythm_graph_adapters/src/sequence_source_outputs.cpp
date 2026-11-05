// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status rate_value(NumericOperationOutput& output,const PreparedOnsetRateRecipe& source,const FrameStamp& stamp) noexcept {
    const auto port=output.port;
    if(port<2)return write_pcm_scalar(output,port?source.duration_seconds():source.rate(),stamp);
    if(port==2)return write_vector(output,source.onset_times(),stamp);
    if(port==3)return write_vector(output,source.hfc_curve(),stamp);
    if(port==4)return write_vector(output,source.complex_curve(),stamp);
    if(port==5)return write_vector(output,source.events().fused_curve(),stamp);
    if(port==6)return write_vector(output,source.events().smoothed_curve(),stamp);
    if(port==7)return write_vector(output,source.events().selection().gated_curve(),stamp);
    if(port==8)return write_vector(output,source.events().selection().thresholds(),stamp);
    return sequence_source_flags(output,source.events().selection().decisions(),stamp);
}
}
Status SequenceSourceOperation::publish_value(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    switch(plan_.kind){
    case SequenceSourceKind::Fusion:case SequenceSourceKind::Selection:case SequenceSourceKind::Events:return publish_onsets(output,stamp);
    case SequenceSourceKind::Rate:return rate_value(output,std::get<PreparedOnsetRateRecipe>(kernel_),stamp);
    case SequenceSourceKind::IntervalSummary:return publish_summary(output,std::get<PreparedBeatIntervalSummary>(kernel_),stamp);
    case SequenceSourceKind::PcmSummary:return publish_summary(output,std::get<PreparedPcmRhythmSummary>(kernel_).summary(),stamp);
    default:return publish_stream(output,stamp);
    }
}
}
