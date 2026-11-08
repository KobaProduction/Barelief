// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/activation_peak_sequence.h>
#include <rhythm_reference_ports/adaptive_onset_selection.h>
#include <rhythm_reference_ports/beat_interval_summary.h>
#include <rhythm_reference_ports/normalized_feature_fusion.h>
#include <rhythm_reference_ports/onset_events.h>
#include <rhythm_reference_ports/spectral_difference_sequence.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
enum class SequenceSourceKind {Fusion,Selection,Events,IntervalSummary,Activation,Difference};
struct SequenceSourceShape {signal_contracts::DType dtype=signal_contracts::DType::Float32;signal_contracts::Rank rank=signal_contracts::Rank::Scalar;std::size_t rows=1,columns=1;bool variable=false;};
struct SequenceSourcePlan {
    SequenceSourceKind kind=SequenceSourceKind::Fusion;
    std::size_t maximum=0,features=1,frames=0,events=0,bins=0,kernel_bytes=0,buffer_bytes=0;
    bool reset_each_call=true;
    float maximum_bpm=1000;
    rhythm_reference_ports::ActivationPeakSequenceConfig activation;
    rhythm_reference_ports::SpectralDifferenceSequenceConfig difference;
    rhythm_reference_ports::AdaptiveOnsetSelectionConfig selection;
};
bool sequence_source_kind(std::string_view) noexcept;
bool sequence_source_plan(const dataflow_runtime::NumericOperationNodeView&,SequenceSourcePlan&) noexcept;
bool sequence_source_settings(const dataflow_runtime::NumericOperationNodeView&,SequenceSourcePlan&) noexcept;
bool sequence_source_storage(const dataflow_runtime::NumericOperationNodeView&,SequenceSourcePlan&) noexcept;
std::size_t sequence_source_input_count(SequenceSourceKind) noexcept;
std::size_t sequence_source_output_count(SequenceSourceKind) noexcept;
SequenceSourceShape sequence_source_shape(const SequenceSourcePlan&,domain::PortId) noexcept;
dataflow_runtime::NumericOperationPolicy sequence_source_policy(const dataflow_runtime::NumericOperationNodeView&);
domain::ProcessingSupport assess_sequence_source(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_sequence_source(const dataflow_runtime::NumericOperationNode&);

class SequenceSourceOperation final:public AdapterOperation {
public:
    bool prepare(const dataflow_runtime::NumericOperationNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    void reset() noexcept override;
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    bool prepare_kernel();
    signal_contracts::Status process_enabled(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept;
    signal_contracts::Status preflight(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) const noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) noexcept;
    bool calculate_onsets(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    bool calculate_stream(std::span<const dataflow_runtime::NumericOperationInput>,std::span<dataflow_runtime::NumericOperationOutput>) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_value(dataflow_runtime::NumericOperationOutput&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_onsets(dataflow_runtime::NumericOperationOutput&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_stream(dataflow_runtime::NumericOperationOutput&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_summary(dataflow_runtime::NumericOperationOutput&,const rhythm_reference_ports::PreparedBeatIntervalSummary&,const dataflow_runtime::NumericOperationOrigin&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationOrigin&) const noexcept;
    SequenceSourcePlan plan_;
    std::variant<rhythm_reference_ports::PreparedNormalizedFeatureFusion,rhythm_reference_ports::PreparedAdaptiveOnsetSelection,
        rhythm_reference_ports::PreparedOnsetEvents,
        rhythm_reference_ports::PreparedBeatIntervalSummary,
        rhythm_reference_ports::PreparedActivationPeakSequence,rhythm_reference_ports::PreparedSpectralDifferenceSequence> kernel_;
    std::vector<float> values_,times_;
    std::vector<std::uint8_t> flags_;
    rhythm_reference_ports::BeatIntervalHistogram histogram_;
    bool histogram_ready_=false;
    std::size_t result_rows_=0,result_features_=0,event_count_=0;
};
}
