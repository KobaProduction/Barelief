// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include <dsp_kernels/frame_timing.h>
#include <algorithm>
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool activation_bound(SequenceSourcePlan& p) noexcept {
    std::size_t pre=0,smooth=0;
    if(!dsp_kernels::rounded_frame_count(p.activation.pre_maximum,p.activation.frame_rate,pre) ||
       !dsp_kernels::rounded_frame_count(p.activation.online?0:p.activation.smooth,p.activation.frame_rate,smooth) || pre>SIZE_MAX-p.frames)return false;
    p.events=std::max(p.frames+pre,smooth);if(p.events==SIZE_MAX)return false;++p.events;
    return PreparedActivationPeakSequence::storage_bytes(p.frames,p.activation,p.kernel_bytes);
}
bool kernel_bound(SequenceSourcePlan& p) noexcept {
    switch(p.kind){
    case SequenceSourceKind::Fusion:return PreparedNormalizedFeatureFusion::storage_bytes(p.features,p.frames,p.kernel_bytes);
    case SequenceSourceKind::Selection:return PreparedAdaptiveOnsetSelection::storage_bytes(p.frames,p.selection,p.kernel_bytes);
    case SequenceSourceKind::Events:return PreparedOnsetEvents::storage_bytes(p.features,p.frames,p.selection,p.kernel_bytes);
    case SequenceSourceKind::IntervalSummary:return PreparedBeatIntervalSummary::storage_bytes(p.maximum,p.maximum_bpm,p.kernel_bytes);
    case SequenceSourceKind::Activation:return activation_bound(p);
    case SequenceSourceKind::Difference:return PreparedSpectralDifferenceSequence::storage_bytes(p.frames,p.features,p.difference,p.kernel_bytes);
    }
    return false;
}
}
bool sequence_source_storage(const dataflow_runtime::NumericOperationNodeView&,SequenceSourcePlan& p) noexcept {
    const auto kind=p.kind;if(!kernel_bound(p))return false;
    if(kind==SequenceSourceKind::Fusion)return dataflow_runtime::numeric_operation_mul_bytes(p.frames,sizeof(float),p.buffer_bytes);
    if(kind==SequenceSourceKind::Selection || kind==SequenceSourceKind::Events)
        return dataflow_runtime::numeric_operation_mul_bytes(p.frames,sizeof(float)+sizeof(std::uint8_t),p.buffer_bytes);
    return true;
}
}
