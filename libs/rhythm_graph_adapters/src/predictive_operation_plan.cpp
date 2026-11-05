// SPDX-License-Identifier: AGPL-3.0-or-later
#include "predictive_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <cmath>
#include <utility>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool parameters(const domain::ResolvedProcessingNode& node,PredictivePlan& plan) noexcept {
    std::size_t hop=512,frame=1024,method=6,window=1;
    if(!integer_parameter(node,"hop_samples",512,hop) || !std::in_range<std::uint32_t>(hop) ||
       !integer_parameter(node,"frame_samples",1024,frame) || !std::in_range<std::uint32_t>(frame) ||
       !integer_parameter(node,"method",6,method) || method>9 || !integer_parameter(node,"window_shape",1,window) || window>4 ||
       !integer_parameter(node,"history_size",512,plan.history) || !scalar_parameter(node,"initial_period",43,plan.period) ||
       !scalar_parameter(node,"tightness",5,plan.tightness) || !scalar_parameter(node,"alpha",.9f,plan.alpha))return false;
    plan.config={std::uint32_t(hop),std::uint32_t(frame),PredictiveOnsetMethod(method),PredictiveWindowShape(window)};
    if(plan.kind==PredictiveKind::Tracker || plan.kind==PredictiveKind::PcmTracker){if(!hop)return false;plan.history=262144/hop;}
    return true;
}
bool source_input(const domain::ResolvedProcessingNode& node,const PredictivePlan& plan) noexcept {
    NumericContract input;if(!numeric_port(node,true,0,input) || input.dtype!=DType::Float32 || input.channel_order.size()!=1)return false;
    const bool pcm=plan.kind==PredictiveKind::Frontend || plan.kind==PredictiveKind::PcmTracker;
    if(pcm || plan.kind==PredictiveKind::Comb){
        const auto rows=pcm?plan.config.hop_samples:512;
        return input.shape.rank==Rank::Vector && input.shape.extents[0]==ExtentBounds{rows,rows} && input.representation.encoding==StorageEncoding::Native &&
            (!pcm || !input.clock.sample_rate || input.clock.sample_rate==44100);
    }
    return input.shape.rank==Rank::Scalar;
}
bool inputs(const domain::ResolvedProcessingNode& node,const PredictivePlan& plan) noexcept {
    if(node.inputs.size()>7 || !source_input(node,plan))return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        const auto id=node.inputs[i].id;NumericContract input;
        if(id && (!numeric_port(node,true,id,input) || !predictive_control_input(input,plan.kind,id)))return false;
        for(std::size_t j=0;j<i;++j)if(node.inputs[j].id==id)return false;
    }
    return true;
}
}
bool predictive_control_input(const NumericContract& input,PredictiveKind kind,domain::PortId port) noexcept {
    if(input.shape.rank!=Rank::Scalar || input.channel_order.size()!=1 || input.representation.normalization!=Normalization::None)return false;
    const bool tracker=kind==PredictiveKind::Comb || kind==PredictiveKind::Tracker || kind==PredictiveKind::PcmTracker;
    if(port==predictive_control_port)return input.dtype==DType::UInt8;
    if(port==10)return (kind==PredictiveKind::Frontend || kind==PredictiveKind::PcmTracker) && input.dtype==DType::UInt32;
    if(port==11 || port==12)return tracker && input.dtype==(port==11?DType::Float32:DType::UInt8);
    return (port==13 || port==14) && kind==PredictiveKind::Prediction && input.dtype==DType::Float32;
}
bool predictive_operation_kind(std::string_view kind) noexcept {
    return kind=="PcmPredictiveOnset" || kind=="CombTempoTransition" || kind=="BeatPrediction" ||
        kind=="PredictiveBeatTracker" || kind=="PcmPredictiveBeatTracker";
}
bool predictive_operation_plan(const domain::ResolvedProcessingNode& node,PredictivePlan& plan) noexcept {
    plan={};if(!predictive_operation_kind(node.kind))return false;
    if(node.kind=="CombTempoTransition")plan.kind=PredictiveKind::Comb;
    else if(node.kind=="BeatPrediction")plan.kind=PredictiveKind::Prediction;
    else if(node.kind=="PredictiveBeatTracker")plan.kind=PredictiveKind::Tracker;
    else if(node.kind=="PcmPredictiveBeatTracker")plan.kind=PredictiveKind::PcmTracker;
    return parameters(node,plan) && inputs(node,plan);
}
bool predictive_operation_storage(PredictivePlan& plan,const dsp_kernels::ResamplerBackendFactory* provider) noexcept {
    using namespace dsp_kernels;std::size_t nested=0;
    switch(plan.kind){
    case PredictiveKind::Frontend:return PreparedPcmPredictiveOnset::storage_bytes(plan.config,plan.kernel_bytes);
    case PredictiveKind::Comb:return PreparedCombTempoTransition::storage_bytes(plan.config.hop_samples,plan.kernel_bytes);
    case PredictiveKind::Prediction:return PreparedBeatPrediction::storage_bytes(plan.history,plan.period,plan.tightness,plan.alpha,plan.kernel_bytes);
    case PredictiveKind::PcmTracker:
        return provider && PreparedPcmPredictiveBeatTracker::storage_bytes(*provider,plan.config,plan.kernel_bytes) &&
            dataflow_runtime::numeric_operation_add_bytes(plan.kernel_bytes,sizeof(PreparedPcmPredictiveBeatTracker));
    case PredictiveKind::Tracker:{
        ResamplerResources resources;
        return provider && PreparedPredictiveBeatTracker::storage_bytes(plan.config.hop_samples,nested) &&
            PreparedResampler::window_resource_requirements({plan.history,512,1},provider,resources) &&
            dataflow_runtime::numeric_operation_add_bytes(nested,resources.owned_bytes) && (plan.kernel_bytes=nested,true);
    }
    }
    return false;
}
}
