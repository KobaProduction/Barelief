// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/beat_interval_histogram.h>
#include <rhythm_reference_ports/harmonic_tempo.h>
#include <rhythm_reference_ports/tempo_change_sections.h>
#include <rhythm_reference_ports/detrended_fluctuation.h>
#include <variant>

namespace rhythm_graph_adapters::detail {
enum class IntervalOperationKind {Histogram,Harmonics,Sections,Fluctuation};
class IntervalOperation final:public AdapterOperation {
public:
    bool prepare(const domain::ConfiguredProcessingNode&);
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override;
    signal_contracts::Status copy_state_from(const dataflow_runtime::PreparedNumericOperation&) noexcept override;
    // These kernels compute independent complete batches, with no analytical
    // history between calls. Output availability belongs to the runtime.
    void reset() noexcept override {}
    signal_contracts::Status process(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,dataflow_runtime::NumericOperationControl) noexcept override;
    std::size_t workspace_bytes() const noexcept override;
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
private:
    bool prepare_harmonics(const domain::ConfiguredProcessingNode&);
    bool prepare_sections(const domain::ConfiguredProcessingNode&);
    bool prepare_fluctuation(const domain::ConfiguredProcessingNode&);
    bool output_geometry(const domain::ConfiguredProcessingNode&) const noexcept;
    bool calculate(std::span<const float>) noexcept;
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,const dataflow_runtime::NumericOperationInput*&) const noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_histogram(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_fluctuation(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    std::array<std::size_t,2> disabled_shape(domain::PortId) const noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    IntervalOperationKind kind_=IntervalOperationKind::Histogram;
    std::size_t maximum_=0;
    rhythm_reference_ports::BeatIntervalHistogram histogram_;
    std::variant<std::monostate,rhythm_reference_ports::PreparedHarmonicTempoCandidates,
        rhythm_reference_ports::PreparedTempoChangeSections,rhythm_reference_ports::PreparedDetrendedFluctuation> kernel_;
};
}
