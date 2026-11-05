// SPDX-License-Identifier: AGPL-3.0-or-later
#include "predictive_operations.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status prediction_value(NumericOperationOutput& output,domain::PortId port,const BeatPredictionObservation& value,const FrameStamp& stamp) noexcept {
    if(!port)return write_pcm_scalar(output,std::uint8_t(value.beat_due),stamp);
    if(port==1)return write_pcm_scalar(output,std::int64_t(value.next_beat),stamp);
    if(port==2)return write_pcm_scalar(output,std::int64_t(value.next_prediction),stamp);
    return write_pcm_scalar(output,value.period_frames,stamp);
}
}
Status PredictiveOperation::publish_frontend(NumericOperationOutput& output,domain::PortId port,
    const PreparedPcmPredictiveOnset& source,const FrameStamp& stamp) noexcept {
    if(!port)return write_pcm_scalar(output,value_,stamp);
    const bool energy=source.config().method==PredictiveOnsetMethod::EnergyEnvelope || source.config().method==PredictiveOnsetMethod::EnergyDifference;
    if(energy && port<4)return Status::ok; // This source branch did not run FFT.
    if(port==1)return write_pcm_complex(output,source.complex_values(),stamp);
    if(port==2)return write_vector(output,source.magnitude(),stamp);
    if(port==3)return write_vector(output,source.phase(),stamp);
    if(port==4)return write_vector(output,source.raw_pcm_history(),stamp);
    return write_vector(output,source.spectrum_stage().window_coefficients(),stamp);
}
Status PredictiveOperation::publish_tracker(NumericOperationOutput& output,domain::PortId port,
    const PreparedPredictiveBeatTracker& source,const FrameStamp& stamp) noexcept {
    if(port<4)return prediction_value(output,port,prediction_,stamp);
    if(port==4)return write_pcm_scalar(output,source.bpm(),stamp);
    if(port==5)return write_vector(output,source.onset_window(),stamp);
    if(port==6)return write_vector(output,source.cumulative_scores(),stamp);
    if(port==7)return write_vector(output,source.resampled_onsets(),stamp);
    if(port==8)return write_vector(output,source.tempo_stage().response(),stamp);
    if(port==9)return write_vector(output,source.tempo_stage().tempo_observations(),stamp);
    return write_vector(output,source.tempo_stage().state_probabilities(),stamp);
}
Status PredictiveOperation::publish_value(NumericOperationOutput& output,const FrameStamp& stamp) noexcept {
    const auto port=output.port;
    if(plan_.kind==PredictiveKind::Frontend)return publish_frontend(output,port,std::get<PreparedPcmPredictiveOnset>(kernel_),stamp);
    if(plan_.kind==PredictiveKind::PcmTracker){
        if(!port)return write_pcm_scalar(output,value_,stamp);
        return port<12?publish_tracker(output,domain::PortId(port-1),pcm_->tracker_stage(),stamp):
            publish_frontend(output,domain::PortId(port-11),pcm_->frontend(),stamp);
    }
    if(plan_.kind==PredictiveKind::Tracker)return publish_tracker(output,port,std::get<PreparedPredictiveBeatTracker>(kernel_),stamp);
    if(plan_.kind==PredictiveKind::Prediction)return port<4?prediction_value(output,port,prediction_,stamp):
        write_vector(output,std::get<PreparedBeatPrediction>(kernel_).cumulative_scores(),stamp);
    const auto& source=std::get<PreparedCombTempoTransition>(kernel_);
    if(port<2)return write_pcm_scalar(output,port?source.bpm():value_,stamp);
    return write_vector(output,port==2?source.response():port==3?source.tempo_observations():source.state_probabilities(),stamp);
}
Status PredictiveOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    for(auto& output:outputs){
        auto status=publish_value(output,stamp);if(status!=Status::ok)return status;
        if(output.status==Status::ok)status=validate_values(output.value.view(),ValueDomain::FiniteOnly,limits_);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
