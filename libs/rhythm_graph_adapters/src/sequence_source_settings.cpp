// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool flag(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key,bool fallback,bool& output) noexcept {
    const auto* item=parameter(node,key);if(!item){output=fallback;return true;}
    const auto* value=std::get_if<bool>(item);if(!value)return false;output=*value;return true;
}
bool selection_settings(const dataflow_runtime::NumericOperationNodeView& node,AdaptiveOnsetSelectionConfig& c) noexcept {
    return integer_parameter(node,"delay",5,c.delay) && scalar_parameter(node,"frame_rate",44100.f/512.f,c.frame_rate) &&
        scalar_parameter(node,"alpha",.1f,c.alpha) && scalar_parameter(node,"silence_threshold",.02f,c.silence_threshold);
}
bool activation_settings(const dataflow_runtime::NumericOperationNodeView& n,ActivationPeakSequenceConfig& c) noexcept {
    return scalar_parameter(n,"frame_rate",100,c.frame_rate) && scalar_parameter(n,"threshold",.5f,c.threshold) &&
        scalar_parameter(n,"smooth",0,c.smooth) && scalar_parameter(n,"pre_average",0,c.pre_average) && scalar_parameter(n,"post_average",0,c.post_average) &&
        scalar_parameter(n,"pre_maximum",0,c.pre_maximum) && scalar_parameter(n,"post_maximum",0,c.post_maximum) &&
        scalar_parameter(n,"combine",.03f,c.combine) && scalar_parameter(n,"delay",0,c.delay) && flag(n,"online",false,c.online);
}
bool difference_settings(const dataflow_runtime::NumericOperationNodeView& n,SpectralDifferenceSequenceConfig& c) noexcept {
    return integer_parameter(n,"lag",c.lag,c.lag) && integer_parameter(n,"frequency_maximum",c.frequency_maximum,c.frequency_maximum) && flag(n,"positive",c.positive,c.positive);
}
bool remaining_settings(const dataflow_runtime::NumericOperationNodeView& n,SequenceSourcePlan& p) noexcept {
    if(p.kind==SequenceSourceKind::Difference)return difference_settings(n,p.difference);
    if(p.kind==SequenceSourceKind::IntervalSummary)return scalar_parameter(n,"maximum_bpm",1000,p.maximum_bpm);
    return false;
}
}
bool sequence_source_settings(const dataflow_runtime::NumericOperationNodeView& n,SequenceSourcePlan& p) noexcept {
    if(!flag(n,"reset_each_call",true,p.reset_each_call))return false;
    if(p.kind==SequenceSourceKind::Fusion)return true;
    if(p.kind==SequenceSourceKind::Selection || p.kind==SequenceSourceKind::Events)return selection_settings(n,p.selection);
    if(p.kind==SequenceSourceKind::Activation)return activation_settings(n,p.activation);
    return remaining_settings(n,p);
}
}
