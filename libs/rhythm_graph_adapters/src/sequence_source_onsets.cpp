// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
bool SequenceSourceOperation::calculate_onsets(std::span<const NumericOperationInput> inputs) noexcept {
    result_rows_=inputs[0].value.actual[0];event_count_=0;
    if(plan_.kind==SequenceSourceKind::Fusion || plan_.kind==SequenceSourceKind::Events){
        result_features_=inputs[0].value.actual[0];result_rows_=inputs[0].value.actual[1];
        const auto weights=inputs[1].contiguous_float32;
        if(plan_.kind==SequenceSourceKind::Fusion)return std::get<PreparedNormalizedFeatureFusion>(kernel_).process(
            sequence_source_rows(inputs[0]),result_features_,result_rows_,weights,std::span(values_).first(result_rows_));
        auto& source=std::get<PreparedOnsetEvents>(kernel_);
        return source.process(sequence_source_rows(inputs[0]),result_features_,result_rows_,weights,std::span(flags_).first(result_rows_),plan_.reset_each_call) &&
            source.onset_times(times_,event_count_);
    }
    float weight=0;if(load_element(inputs[1].value,{0,0,0},weight)!=Status::ok)return false;
    auto& source=std::get<PreparedAdaptiveOnsetSelection>(kernel_);
    return source.process(inputs[0].contiguous_float32,weight,std::span(flags_).first(result_rows_)) && source.onset_times(times_,event_count_);
}
bool SequenceSourceOperation::calculate(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept {
    histogram_ready_=false;
    if(plan_.kind==SequenceSourceKind::Fusion || plan_.kind==SequenceSourceKind::Selection || plan_.kind==SequenceSourceKind::Events)return calculate_onsets(inputs);
    if(plan_.kind==SequenceSourceKind::Activation || plan_.kind==SequenceSourceKind::Difference || plan_.kind==SequenceSourceKind::PcmDifference || plan_.kind==SequenceSourceKind::Trajectory)return calculate_stream(inputs,outputs);
    if(plan_.kind==SequenceSourceKind::Rate)return std::get<PreparedOnsetRateRecipe>(kernel_).process(inputs[0].contiguous_float32);
    if(plan_.kind==SequenceSourceKind::PcmSummary){
        auto& source=std::get<PreparedPcmRhythmSummary>(kernel_);if(!source.process(inputs[0].contiguous_float32))return false;
        histogram_ready_=source.histogram_descriptors(histogram_);return true;
    }
    float confidence=0;if(load_element(inputs[1].value,{0,0,0},confidence)!=Status::ok)return false;
    auto& source=std::get<PreparedBeatIntervalSummary>(kernel_);if(!source.process(inputs[0].contiguous_float32,confidence))return false;
    histogram_ready_=source.histogram_descriptors(histogram_);return true;
}
}
