// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include <dsp_kernels/frame_timing.h>
#include <algorithm>
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool frame_count(std::size_t maximum,std::size_t size,std::size_t hop,bool center,std::size_t& result) noexcept {
    dsp_kernels::TemporalWindowConfig window;window.length=size;window.hop=hop;window.periodic=false;window.center=center;window.odd_center_ceil=true;
    window.tail=center?dsp_kernels::TemporalTail::CenterCoverEnd:dsp_kernels::TemporalTail::CoverEnd;
    return dsp_kernels::PreparedTemporalWindow::frame_count(window,maximum,result);
}
bool activation_bound(SequenceSourcePlan& p) noexcept {
    std::size_t pre=0,smooth=0;
    if(!dsp_kernels::rounded_frame_count(p.activation.pre_maximum,p.activation.frame_rate,pre) ||
       !dsp_kernels::rounded_frame_count(p.activation.online?0:p.activation.smooth,p.activation.frame_rate,smooth) || pre>SIZE_MAX-p.frames)return false;
    p.events=std::max(p.frames+pre,smooth);if(p.events==SIZE_MAX)return false;++p.events;
    return PreparedActivationPeakSequence::storage_bytes(p.frames,p.activation,p.kernel_bytes);
}
bool pcm_difference_bound(SequenceSourcePlan& p) noexcept {
    return dsp_kernels::PreparedSpectralFrameRecipe::geometry(p.maximum,p.pcm_difference.spectrum,{},p.frames,p.features) &&
        (p.bins=(p.pcm_difference.spectrum.fft_size?p.pcm_difference.spectrum.fft_size:p.pcm_difference.spectrum.framing.length)/2+
            std::size_t(p.pcm_difference.spectrum.include_nyquist),true) &&
        PreparedPcmSpectralDifference::storage_bytes(p.maximum,p.pcm_difference,{},{},p.kernel_bytes);
}
bool trajectory_bound(SequenceSourcePlan& p) noexcept {
    p.features=trajectory_band_edges().size()-2;p.bins=p.trajectory.frame_size/2+1;
    return frame_count(p.maximum,p.trajectory.frame_size,p.trajectory.audio_hop,true,p.frames) &&
        PreparedTrajectoryOnsetRecipe::storage_bytes(p.maximum,p.events,p.trajectory,p.kernel_bytes);
}
bool kernel_bound(SequenceSourcePlan& p) noexcept {
    switch(p.kind){
    case SequenceSourceKind::Fusion:return PreparedNormalizedFeatureFusion::storage_bytes(p.features,p.frames,p.kernel_bytes);
    case SequenceSourceKind::Selection:return PreparedAdaptiveOnsetSelection::storage_bytes(p.frames,p.selection,p.kernel_bytes);
    case SequenceSourceKind::Events:return PreparedOnsetEvents::storage_bytes(p.features,p.frames,p.selection,p.kernel_bytes);
    case SequenceSourceKind::Rate:return frame_count(p.maximum,1024,512,p.rate==OnsetRateRecipe::Streaming,p.frames) && PreparedOnsetRateRecipe::storage_bytes(p.maximum,p.rate,p.kernel_bytes);
    case SequenceSourceKind::IntervalSummary:return PreparedBeatIntervalSummary::storage_bytes(p.maximum,p.maximum_bpm,p.kernel_bytes);
    case SequenceSourceKind::PcmSummary:return PreparedPcmRhythmSummary::maximum_tick_count(p.maximum,p.beat,p.events) && PreparedPcmRhythmSummary::storage_bytes(p.maximum,p.beat,p.kernel_bytes);
    case SequenceSourceKind::Activation:return activation_bound(p);
    case SequenceSourceKind::Difference:return PreparedSpectralDifferenceSequence::storage_bytes(p.frames,p.features,p.difference,p.kernel_bytes);
    case SequenceSourceKind::PcmDifference:return pcm_difference_bound(p);
    case SequenceSourceKind::Trajectory:return trajectory_bound(p);
    }
    return false;
}
}
bool sequence_source_storage(const domain::ResolvedProcessingNode&,SequenceSourcePlan& p) noexcept {
    const auto kind=p.kind;if(!kernel_bound(p))return false;
    if(kind==SequenceSourceKind::Fusion)return dataflow_runtime::numeric_operation_mul_bytes(p.frames,sizeof(float),p.buffer_bytes);
    if(kind==SequenceSourceKind::Selection || kind==SequenceSourceKind::Events)
        return dataflow_runtime::numeric_operation_mul_bytes(p.frames,sizeof(float)+sizeof(std::uint8_t),p.buffer_bytes);
    return true;
}
}
