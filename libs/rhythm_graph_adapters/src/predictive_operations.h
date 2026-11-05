// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/pcm_predictive_beat_tracker.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId predictive_control_port=65500;
enum class PredictiveKind {Frontend,Comb,Prediction,Tracker,PcmTracker};
struct PredictiveWarmControls {
    std::optional<std::uint32_t> method;
    std::optional<float> tempo,period,indicated_period;
    std::optional<bool> fixed;
};
struct PredictivePlan {
    PredictiveKind kind=PredictiveKind::Frontend;
    rhythm_reference_ports::PcmPredictiveOnsetConfig config;
    std::size_t history=512,kernel_bytes=0;
    float period=43,tightness=5,alpha=.9f;
};
struct PredictiveShape {signal_contracts::DType dtype=signal_contracts::DType::Float32;signal_contracts::Rank rank=signal_contracts::Rank::Scalar;std::size_t rows=1,columns=1;};
bool predictive_operation_kind(std::string_view) noexcept;
bool predictive_operation_plan(const domain::ResolvedProcessingNode&,PredictivePlan&) noexcept;
bool predictive_control_input(const signal_contracts::NumericContract&,PredictiveKind,domain::PortId) noexcept;
std::size_t predictive_output_count(PredictiveKind) noexcept;
PredictiveShape predictive_shape(const PredictivePlan&,domain::PortId) noexcept;
bool predictive_operation_storage(PredictivePlan&,const dsp_kernels::ResamplerBackendFactory*) noexcept;
dataflow_runtime::NumericOperationPolicy predictive_operation_policy(const domain::ResolvedProcessingNode&);
domain::ProcessingSupport assess_predictive_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&,const dsp_kernels::ResamplerBackendFactory*) noexcept;
std::unique_ptr<AdapterOperation> prepare_predictive_operation(const domain::ConfiguredProcessingNode&,std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>);

class PredictiveOperation final:public AdapterOperation {
public:
    bool prepare(const domain::ConfiguredProcessingNode&,std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    static bool resample(void*,std::span<const float>,std::span<float>) noexcept;
    bool prepare_kernel();
    bool calculate(const dataflow_runtime::NumericOperationInput&) noexcept;
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,bool allow_empty,bool& has_frame,PredictiveWarmControls&) const noexcept;
    signal_contracts::Status read_controls(std::span<const dataflow_runtime::NumericOperationInput>,PredictiveWarmControls&) const noexcept;
    bool apply_controls(const PredictiveWarmControls&) noexcept;
    bool apply_tempo_controls(const PredictiveWarmControls&) noexcept;
    bool apply_prediction_controls(const PredictiveWarmControls&) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_frontend(dataflow_runtime::NumericOperationOutput&,domain::PortId,const rhythm_reference_ports::PreparedPcmPredictiveOnset&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_tracker(dataflow_runtime::NumericOperationOutput&,domain::PortId,const rhythm_reference_ports::PreparedPredictiveBeatTracker&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_value(dataflow_runtime::NumericOperationOutput&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) const noexcept;
    PredictivePlan plan_;
    std::variant<std::monostate,rhythm_reference_ports::PreparedPcmPredictiveOnset,rhythm_reference_ports::PreparedCombTempoTransition,
        rhythm_reference_ports::PreparedBeatPrediction,rhythm_reference_ports::PreparedPredictiveBeatTracker> kernel_;
    std::unique_ptr<rhythm_reference_ports::PreparedPcmPredictiveBeatTracker> pcm_;
    std::unique_ptr<dsp_kernels::PreparedResampler> window_;
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> provider_;
    rhythm_reference_ports::BeatPredictionObservation prediction_;
    rhythm_reference_ports::PcmPredictiveBeatObservation pcm_result_;
    PredictiveWarmControls applied_;
    float value_=0;
};
}
