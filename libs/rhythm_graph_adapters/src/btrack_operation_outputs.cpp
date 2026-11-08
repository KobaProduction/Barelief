// SPDX-License-Identifier: AGPL-3.0-or-later
#include "btrack_operations.h"
#include "operation_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
using namespace rhythm_reference_ports;
namespace {
namespace bp=rhythm_graph_adapters::btrack_ports;
using DecisionOutput=bp::DecisionOutput;
float decision_scalar(const BTrackDecision& stage,domain::PortId port) noexcept {
    if(port==bp::port(DecisionOutput::BeatFlag))return stage.result.beat_due?1.f:0.f;
    if(port==bp::port(DecisionOutput::TempoBpm))return stage.tempo.bpm();
    return port==bp::port(DecisionOutput::PeriodFrames)?stage.result.period_frames:stage.prediction.latest_cumulative_score();
}
Status decision_value(NumericOperationOutput& output,const BTrackDecision& stage,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    if(output.port==bp::port(DecisionOutput::CumulativeHistory))return write_vector(output,stage.prediction.cumulative_scores(),stamp);
    if(output.port==bp::port(DecisionOutput::TempoProbabilities))return write_vector(output,stage.tempo.state_probabilities(),stamp);
    if(output.port==bp::port(DecisionOutput::NextBeat) || output.port==bp::port(DecisionOutput::NextPrediction))return write_pcm_scalar(output,
        std::int64_t(output.port==bp::port(DecisionOutput::NextBeat)?stage.result.next_beat:stage.result.next_prediction),stamp);
    return write_scalar(output,decision_scalar(stage,output.port),stamp);
}
}
Status BTrackOperation::publish_decision(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    const auto& stage=std::get<BTrackDecision>(kernel_);
    for(auto& output:outputs){
        const auto status=decision_value(output,stage,stamp);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status BTrackOperation::publish(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    if(plan_.kind==BTrackKind::Decision)return publish_decision(outputs,stamp);
    if(plan_.kind==BTrackKind::Onset)
        return write_pcm_scalar(outputs.front(),std::get<BTrackSourceOnset>(kernel_).kernel.value(),stamp);
    for(auto& output:outputs){
        const auto status=publish_value(output,stamp);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status BTrackOperation::publish_value(NumericOperationOutput& output,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    if(plan_.kind==BTrackKind::Comb || plan_.kind==BTrackKind::Threshold || plan_.kind==BTrackKind::Response || plan_.kind==BTrackKind::Resample){
        const auto shape=btrack_shape(plan_.kind,output.port,plan_.size);
        return write_vector(output,std::span(resampled_).first(shape.count),stamp);
    }
    if(plan_.kind==BTrackKind::History && output.port==bp::port(bp::HistoryOutput::OnsetWindow))return write_vector(output,std::get<ScalarHistory>(kernel_).values(),stamp);
    return write_scalar(output,scalar_,stamp);
}
}
