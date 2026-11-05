// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
bool SequenceSourceOperation::prepare_kernel() {
    switch(plan_.kind){
    case SequenceSourceKind::Fusion:return kernel_.emplace<PreparedNormalizedFeatureFusion>().prepare(plan_.features,plan_.frames);
    case SequenceSourceKind::Selection:return kernel_.emplace<PreparedAdaptiveOnsetSelection>().prepare(plan_.frames,plan_.selection);
    case SequenceSourceKind::Events:return kernel_.emplace<PreparedOnsetEvents>().prepare(plan_.features,plan_.frames,plan_.selection);
    case SequenceSourceKind::Rate:return kernel_.emplace<PreparedOnsetRateRecipe>().prepare(plan_.maximum,plan_.rate);
    case SequenceSourceKind::IntervalSummary:return kernel_.emplace<PreparedBeatIntervalSummary>().prepare(plan_.maximum,plan_.maximum_bpm);
    case SequenceSourceKind::PcmSummary:return kernel_.emplace<PreparedPcmRhythmSummary>().prepare(plan_.maximum,plan_.beat);
    case SequenceSourceKind::Activation:return kernel_.emplace<PreparedActivationPeakSequence>().prepare(plan_.frames,plan_.activation);
    case SequenceSourceKind::Difference:return kernel_.emplace<PreparedSpectralDifferenceSequence>().prepare(plan_.frames,plan_.features,plan_.difference);
    case SequenceSourceKind::PcmDifference:return kernel_.emplace<PreparedPcmSpectralDifference>().prepare(plan_.maximum,plan_.pcm_difference);
    case SequenceSourceKind::Trajectory:return kernel_.emplace<PreparedTrajectoryOnsetRecipe>().prepare(plan_.maximum,plan_.events,plan_.trajectory);
    }
    return false;
}
bool SequenceSourceOperation::prepare(const domain::ConfiguredProcessingNode& node) {
    if(!sequence_source_plan(definition_view(node),plan_) || !prepare_kernel())return false;
    if(plan_.kind==SequenceSourceKind::Fusion)values_.resize(plan_.frames);
    else if(plan_.kind==SequenceSourceKind::Selection || plan_.kind==SequenceSourceKind::Events){flags_.resize(plan_.frames);times_.resize(plan_.frames);}
    std::size_t bytes=plan_.kernel_bytes;
    return dataflow_runtime::numeric_operation_add_bytes(bytes,plan_.buffer_bytes) && workspace_bytes()<=bytes;
}
std::unique_ptr<AdapterOperation> prepare_sequence_source(const domain::ConfiguredProcessingNode& node) {
    auto result=std::make_unique<SequenceSourceOperation>();if(!result->prepare(node))return {};return result;
}
std::unique_ptr<dataflow_runtime::PreparedNumericOperation> SequenceSourceOperation::clone() const {return std::make_unique<SequenceSourceOperation>(*this);}
std::size_t SequenceSourceOperation::workspace_bytes() const noexcept {
    return std::visit([](const auto& source){return source.workspace_bytes();},kernel_)+
        (values_.capacity()+times_.capacity())*sizeof(float)+flags_.capacity();
}
void SequenceSourceOperation::reset() noexcept {
    std::visit([](auto& source){
        using T=std::decay_t<decltype(source)>;
        if constexpr(!std::is_same_v<T,PreparedBeatIntervalSummary> && !std::is_same_v<T,PreparedPcmRhythmSummary>)source.reset();
    },kernel_);
    result_rows_=0;result_features_=0;event_count_=0;histogram_ready_=false;
}
signal_contracts::Status SequenceSourceOperation::copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept {
    reset();return signal_contracts::Status::incompatible_contract;
}
}
