// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <dsp_kernels/frame_timing.h>
#include <algorithm>
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
bool identify(std::string_view name,SequenceSourceKind& kind) noexcept {
    constexpr std::array<std::string_view,6> names{"NormalizedFeatureFusion","AdaptiveOnsetSelection","OnsetEvents",
        "BeatIntervalSummary","ActivationPeakSequence","SpectralDifferenceSequence"};
    for(std::size_t i=0;i<names.size();++i)if(name==names[i]){kind=SequenceSourceKind(i);return true;}return false;
}
void matrix_geometry(const NumericBufferContract& input,SequenceSourcePlan& plan) noexcept {
    if(plan.kind==SequenceSourceKind::Difference)plan.features=std::size_t(input.shape.extents[1].maximum);
    else{plan.features=plan.maximum;plan.frames=std::size_t(input.shape.extents[1].maximum);}
}
bool ordered_inputs(dataflow_runtime::NumericOperationPorts ports) noexcept {
    for(std::size_t i=0;i<ports.size();++i)if(ports[i].id!=i)return false;
    return true;
}
bool source_geometry(const dataflow_runtime::NumericOperationNodeView& n,SequenceSourcePlan& p) noexcept {
    NumericBufferContract input;if(!numeric_port(n,true,0,input) || input.dtype!=DType::Float32 || input.plane_count!=1)return false;
    const bool matrix=p.kind==SequenceSourceKind::Fusion || p.kind==SequenceSourceKind::Events || p.kind==SequenceSourceKind::Difference;
    if(input.shape.rank!=(matrix?Rank::Matrix:Rank::Vector) || input.shape.extents[0].maximum>SIZE_MAX || input.shape.extents[1].maximum>SIZE_MAX)return false;
    p.maximum=std::size_t(input.shape.extents[0].maximum);p.frames=p.maximum;
    if(matrix)matrix_geometry(input,p);
    else if(input.representation.encoding!=StorageEncoding::Native)return false;
    return p.maximum && p.frames && p.features;
}
bool secondary_geometry(const dataflow_runtime::NumericOperationNodeView& n,const SequenceSourcePlan& p) noexcept {
    if(n.inputs.size()!=sequence_source_input_count(p.kind) || !ordered_inputs(n.inputs))return false;
    if(n.inputs.size()==1)return true;
    NumericBufferContract input;if(!numeric_port(n,true,1,input) || input.dtype!=DType::Float32 || input.plane_count!=1)return false;
    if(p.kind==SequenceSourceKind::Selection || p.kind==SequenceSourceKind::IntervalSummary)return input.shape.rank==Rank::Scalar;
    return input.shape.rank==Rank::Vector && input.representation.encoding==StorageEncoding::Native && input.shape.extents[0].maximum==p.features;
}
}
bool sequence_source_kind(std::string_view name) noexcept {SequenceSourceKind kind;return identify(name,kind);}
std::size_t sequence_source_input_count(SequenceSourceKind kind) noexcept {
    return kind==SequenceSourceKind::Fusion || kind==SequenceSourceKind::Selection || kind==SequenceSourceKind::Events || kind==SequenceSourceKind::IntervalSummary?2:1;
}
bool sequence_source_plan(const dataflow_runtime::NumericOperationNodeView& n,SequenceSourcePlan& p) noexcept {
    p={};return identify(n.kind,p.kind) && source_geometry(n,p) && sequence_source_settings(n,p) && secondary_geometry(n,p) && sequence_source_storage(n,p);
}
}
