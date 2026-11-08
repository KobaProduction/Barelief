// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_graph_adapters/btrack_ports.h>
#include <dsp_kernels/prepared_resampler.h>
#include <rhythm_reference_ports/predictive_beat_tracker.h>
#include <rhythm_reference_ports/detail/predictive_reference_frontend.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
enum class BTrackKind {Onset,History,Resample,Comb,Decision,Threshold,Response};
struct BTrackPlan {BTrackKind kind=BTrackKind::Onset;std::size_t bytes=0;std::size_t size=512;};
struct BTrackShape {signal_contracts::Rank rank=signal_contracts::Rank::Scalar;std::size_t count=1;signal_contracts::DType dtype=signal_contracts::DType::Float32;};
// libsamplerate's actual arithmetic profile, Float32 I/O and coefficients,
// Double coefficient interpolation/serial accumulation inside the backend.
// This is independent of the Float32 ODF, comb and prediction analytics.
inline dsp_kernels::ResamplingWindowConfig btrack_sinc_config() noexcept {
    return {512,512,1,dsp_kernels::ResamplingWindowMode::SincBestQuality,1,dsp_kernels::NumericalPrecision::DesktopReference};
}
bool btrack_operation_kind(std::string_view) noexcept;
bool btrack_plan(const dataflow_runtime::NumericOperationNodeView&,BTrackPlan&) noexcept;
std::size_t btrack_outputs(BTrackKind) noexcept;
BTrackShape btrack_shape(BTrackKind,domain::PortId,std::size_t size=512) noexcept;
bool btrack_storage(BTrackPlan&,const dsp_kernels::ResamplerBackendFactory*) noexcept;
dataflow_runtime::NumericOperationPolicy btrack_policy(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_btrack_operation(const dataflow_runtime::NumericOperationNodeView&,
    const dataflow_runtime::NumericOperationProfile&,const dsp_kernels::ResamplerBackendFactory*) noexcept;
std::unique_ptr<AdapterOperation> prepare_btrack_operation(const dataflow_runtime::NumericOperationNode&,
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>);
struct BTrackDecision {
    rhythm_reference_ports::PreparedCombTempoTransition tempo;
    rhythm_reference_ports::PreparedBeatPrediction prediction;
    rhythm_reference_ports::BeatPredictionObservation result;
    bool prepare();
    void reset() noexcept;
    bool process(float,std::span<const float>) noexcept;
    std::size_t workspace_bytes() const noexcept {return tempo.workspace_bytes()+prediction.workspace_bytes();}
};
struct BTrackSourceOnset {
    rhythm_reference_ports::detail::PreparedPredictiveReferenceFrontend kernel;
    std::array<float,2048> complex{};
    bool prepare(){return kernel.prepare_complex(1024);}
    void reset() noexcept {kernel.restart();complex.fill(0);}
    std::size_t workspace_bytes() const noexcept {return kernel.workspace_bytes();}
    bool process(std::span<const float> real,std::span<const float> imaginary,float& output) noexcept;
};
class BTrackOperation final:public AdapterOperation {
public:
    bool prepare(const dataflow_runtime::NumericOperationNode&,std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    bool prepare_kernel();
    bool prepare_onset();
    bool prepare_history();
    bool push_history(float) noexcept;
    bool calculate_history(const dataflow_runtime::NumericOperationInput&) noexcept;
    bool resample_history(std::span<const float>) noexcept;
    signal_contracts::Status publish_value(dataflow_runtime::NumericOperationOutput&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    bool calculate_onset(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    bool calculate_decision(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    signal_contracts::Status validate(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    bool valid_buffers(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    signal_contracts::Status process_enabled(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,
        const dataflow_runtime::NumericOperationOrigin&,dataflow_runtime::NumericOperationControl) noexcept;
    signal_contracts::Status finish(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_decision(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status disabled(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) const noexcept;
    BTrackPlan plan_;
    std::variant<std::monostate,rhythm_reference_ports::ScalarHistory,
        rhythm_reference_ports::PreparedBTrackCombResponse,BTrackDecision,BTrackSourceOnset> kernel_;
    std::unique_ptr<dsp_kernels::PreparedResampler> resampler_;
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> provider_;
    std::array<float,512> resampled_{};
    float scalar_=0;
};
}
