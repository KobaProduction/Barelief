// SPDX-License-Identifier: AGPL-3.0-or-later
#include "btrack_operations.h"
#include "operation_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
using namespace rhythm_reference_ports;
namespace {
float decision_scalar(const BTrackDecision& stage,domain::PortId port) noexcept {
    if(port==0)return stage.result.beat_due?1.f:0.f;
    if(port==1)return stage.tempo.bpm();
    return port==2?stage.result.period_frames:stage.prediction.latest_cumulative_score();
}
Status decision_value(NumericOperationOutput& output,const BTrackDecision& stage,const FrameStamp& stamp) noexcept {
    if(output.port==4)return write_vector(output,stage.prediction.cumulative_scores(),stamp);
    if(output.port==7)return write_vector(output,stage.tempo.state_probabilities(),stamp);
    if(output.port==5 || output.port==6)return write_pcm_scalar(output,
        std::int64_t(output.port==5?stage.result.next_beat:stage.result.next_prediction),stamp);
    return write_scalar(output,decision_scalar(stage,output.port),stamp);
}
}
Status BTrackOperation::publish_decision(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    const auto& stage=std::get<BTrackDecision>(kernel_);
    for(auto& output:outputs){
        const auto status=decision_value(output,stage,stamp);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status BTrackOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    if(plan_.kind==BTrackKind::Decision)return publish_decision(outputs,stamp);
    if(plan_.kind==BTrackKind::Onset && plan_.cartesian_input)
        return write_pcm_scalar(outputs.front(),std::get<BTrackSourceOnset>(kernel_).kernel.value(),stamp);
    for(auto& output:outputs){
        const auto status=publish_value(output,stamp);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status BTrackOperation::publish_value(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    if(plan_.kind==BTrackKind::WindowProduct)return write_vector(output,std::get<BTrackWindowProduct>(kernel_).values,stamp);
    if(plan_.kind==BTrackKind::Comb){
        const auto& stage=std::get<PreparedCombTempoTransition>(kernel_);
        const auto values=output.port==0?stage.tempo_observations():output.port==1?stage.response():stage.balanced_autocorrelation();
        return write_vector(output,values,stamp);
    }
    if(plan_.kind==BTrackKind::Resample)return write_vector(output,resampled_,stamp);
    if(plan_.kind==BTrackKind::History && output.port==1)return write_vector(output,std::get<ScalarHistory>(kernel_).values(),stamp);
    return write_scalar(output,scalar_,stamp);
}
}
