// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_graph_adapters/stage_ports.h>
#include <rhythm_reference_ports/context_beat_sequence.h>
#include <variant>
#include <optional>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId pcm_control_port=65500;
enum class PcmStageKind {Tracker,Sequence};
struct PcmStagePlan {
    PcmStageKind kind=PcmStageKind::Sequence;
    rhythm_reference_ports::ContextBeatSequenceConfig sequence;
    rhythm_reference_ports::ContextBeatTrackerConfig tracker;
    std::size_t input_rows=0,history=0,kernel_bytes=0,buffer_bytes=0;
};
struct PcmStageOutputSpec {
    signal_contracts::DType dtype=signal_contracts::DType::Float32;
    signal_contracts::Rank rank=signal_contracts::Rank::Scalar;
    std::size_t rows=1,columns=1;
    bool variable=false;
    signal_contracts::ValueDomain values=signal_contracts::ValueDomain::FiniteOnly;
};
struct PcmStageControls {
    std::optional<std::int32_t> delay_signed;
    std::optional<unsigned> signature;
    bool poll_tatum=false;
};
bool pcm_stage_operation_kind(std::string_view) noexcept;
bool plan_pcm_stage(const dataflow_runtime::NumericOperationNodeView&,PcmStagePlan&) noexcept;
bool pcm_stage_settings(const dataflow_runtime::NumericOperationNodeView&,PcmStagePlan&) noexcept;
bool pcm_stage_inputs(const dataflow_runtime::NumericOperationNodeView&,const PcmStagePlan&) noexcept;
std::size_t pcm_stage_output_count(const PcmStagePlan&) noexcept;
PcmStageOutputSpec pcm_stage_output_spec(const PcmStagePlan&,domain::PortId) noexcept;
bool pcm_stage_output_contracts(const dataflow_runtime::NumericOperationNodeView&,const PcmStagePlan&) noexcept;
domain::ProcessingSupport assess_pcm_stage(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy pcm_stage_policy(const dataflow_runtime::NumericOperationNodeView&);
std::unique_ptr<AdapterOperation> prepare_pcm_stage(const dataflow_runtime::NumericOperationNode&);

class PcmStageOperation final:public AdapterOperation {
public:
    bool prepare(const dataflow_runtime::NumericOperationNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    bool prepare_kernel();
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,PcmStageControls&,bool& has_frame,dataflow_runtime::NumericOperationEvent) const noexcept;
    signal_contracts::Status read_controls(std::span<const dataflow_runtime::NumericOperationInput>,PcmStageControls&) const noexcept;
    signal_contracts::Status validate_source_inputs(std::span<const dataflow_runtime::NumericOperationInput>,bool& has_frame,
        dataflow_runtime::NumericOperationEvent) const noexcept;
    signal_contracts::Status validate_source_input(const dataflow_runtime::NumericOperationInput&) const noexcept;
    signal_contracts::Status process_enabled(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_output(std::span<const dataflow_runtime::NumericOperationInput>,dataflow_runtime::NumericOperationOutput&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    bool apply_controls(const PcmStageControls&) noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>,const PcmStageControls&) noexcept;
    bool calculate_sequence(std::span<const dataflow_runtime::NumericOperationInput>,const PcmStageControls&) noexcept;
    signal_contracts::Status prepare_output_views(std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    signal_contracts::Status publish(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_context(dataflow_runtime::NumericOperationOutput&,domain::PortId,
        const rhythm_reference_ports::PreparedContextBeatSequence&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_tracker(dataflow_runtime::NumericOperationOutput&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) const noexcept;
    PcmStagePlan plan_;
    std::variant<rhythm_reference_ports::PreparedContextBeatTracker,
        rhythm_reference_ports::PreparedContextBeatSequence> kernel_;
    std::vector<float> offsets_;
    std::size_t offsets_count_=0;
    unsigned tatum_=0;
    rhythm_reference_ports::ContextBeatFrame beat_;
};
}
