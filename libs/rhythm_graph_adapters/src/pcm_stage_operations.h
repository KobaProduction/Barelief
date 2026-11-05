// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/pcm_onset_sequence.h>
#include <rhythm_reference_ports/pcm_context_tempo.h>
#include <variant>
#include <optional>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId pcm_control_port=65500;
enum class PcmStageKind {Spectrum,Onset,Tempo,Tracker,Sequence};
enum class PcmStageAxis {Index,Frequency,LocalTime};
struct PcmStagePlan {
    PcmStageKind kind=PcmStageKind::Spectrum;
    rhythm_reference_ports::PcmOnsetSequenceConfig onset;
    rhythm_reference_ports::PcmContextTempoConfig tempo;
    rhythm_reference_ports::ContextBeatSequenceConfig sequence;
    rhythm_reference_ports::ContextBeatTrackerConfig tracker;
    std::uint32_t sample_rate=44100;
    std::size_t input_rows=0,bins=0,history=0,kernel_bytes=0,buffer_bytes=0;
};
struct PcmStageOutputSpec {
    signal_contracts::DType dtype=signal_contracts::DType::Float32;
    signal_contracts::Rank rank=signal_contracts::Rank::Scalar;
    std::size_t rows=1,columns=1;
    bool variable=false;
    PcmStageAxis axis=PcmStageAxis::Index;
    signal_contracts::ValueDomain values=signal_contracts::ValueDomain::FiniteOnly;
};
struct PcmStageControls {
    std::optional<float> threshold,silence,compression;
    std::optional<bool> whiten;
    std::optional<std::uint64_t> delay_unsigned,minimum;
    std::optional<std::int32_t> delay_signed;
    std::optional<unsigned> signature,profile,window;
    bool poll_tatum=false;
    std::span<const float> coefficients;
};
bool pcm_stage_operation_kind(std::string_view) noexcept;
bool plan_pcm_stage(const domain::ResolvedProcessingNode&,PcmStagePlan&) noexcept;
bool pcm_stage_settings(const domain::ResolvedProcessingNode&,PcmStagePlan&) noexcept;
bool pcm_stage_inputs(const domain::ResolvedProcessingNode&,const PcmStagePlan&) noexcept;
std::size_t pcm_stage_output_count(const PcmStagePlan&) noexcept;
PcmStageOutputSpec pcm_stage_output_spec(const PcmStagePlan&,domain::PortId) noexcept;
bool pcm_stage_output_contracts(const domain::ResolvedProcessingNode&,const PcmStagePlan&) noexcept;
domain::ProcessingSupport assess_pcm_stage(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy pcm_stage_policy(const domain::ResolvedProcessingNode&);
std::unique_ptr<AdapterOperation> prepare_pcm_stage(const domain::ConfiguredProcessingNode&);

class PcmStageOperation final:public AdapterOperation {
public:
    bool prepare(const domain::ConfiguredProcessingNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    bool prepare_kernel(const domain::ResolvedProcessingNode&);
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,PcmStageControls&,bool& has_frame,dataflow_runtime::NumericOperationEvent) const noexcept;
    signal_contracts::Status read_controls(std::span<const dataflow_runtime::NumericOperationInput>,PcmStageControls&) const noexcept;
    signal_contracts::Status validate_source_inputs(std::span<const dataflow_runtime::NumericOperationInput>,bool& has_frame,
        dataflow_runtime::NumericOperationEvent) const noexcept;
    bool apply_controls(const PcmStageControls&) noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>,const PcmStageControls&) noexcept;
    bool calculate_sequence(std::span<const dataflow_runtime::NumericOperationInput>,const PcmStageControls&) noexcept;
    bool calculate_tempo(std::span<const float>,const PcmStageControls&) noexcept;
    signal_contracts::Status prepare_output_views(std::span<dataflow_runtime::NumericOperationOutput>,bool next_frame) const noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_spectrum(dataflow_runtime::NumericOperationOutput&,domain::PortId,
        const rhythm_reference_ports::PreparedHopSpectrum&,std::span<const float>,std::span<const float>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_onset(dataflow_runtime::NumericOperationOutput&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_tempo(dataflow_runtime::NumericOperationOutput&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_context(dataflow_runtime::NumericOperationOutput&,domain::PortId,
        const rhythm_reference_ports::PreparedContextBeatSequence&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_tracker(dataflow_runtime::NumericOperationOutput&,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) const noexcept;
    PcmStagePlan plan_;
    std::variant<rhythm_reference_ports::PreparedHopSpectrum,rhythm_reference_ports::PreparedPcmOnsetSequence,
        rhythm_reference_ports::PreparedPcmContextTempo,rhythm_reference_ports::PreparedContextBeatTracker,
        rhythm_reference_ports::PreparedContextBeatSequence> kernel_;
    std::vector<float> magnitude_,phase_,offsets_;
    std::size_t offsets_count_=0;
    std::uint64_t samples_=0;
    unsigned tatum_=0;
    rhythm_reference_ports::PcmOnsetObservation onset_;
    rhythm_reference_ports::PcmContextTempoObservation tempo_;
    rhythm_reference_ports::ContextBeatFrame beat_;
};
}
