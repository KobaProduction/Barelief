// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "spectral_operation_plan.h"
#include <optional>
#include <variant>

namespace rhythm_graph_adapters::detail {
struct SpectralWarmControls {
    std::optional<bool> whiten;
    std::optional<float> compression,floor,relax_time;
};
class SpectralOperation final:public AdapterOperation {
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
    bool prepare_kernel();
    signal_contracts::Status validate_call(std::span<const dataflow_runtime::NumericOperationInput>,
        std::span<dataflow_runtime::NumericOperationOutput>,SpectralWarmControls&) const noexcept;
    signal_contracts::Status validate_frame_input(const dataflow_runtime::NumericOperationInput&) const noexcept;
    signal_contracts::Status validate_phase_pair(std::span<const dataflow_runtime::NumericOperationInput>,
        const dataflow_runtime::NumericOperationInput&) const noexcept;
    signal_contracts::Status read_controls(std::span<const dataflow_runtime::NumericOperationInput>,SpectralWarmControls&) const noexcept;
    bool apply_controls(const SpectralWarmControls&) noexcept;
    bool calculate(std::span<const dataflow_runtime::NumericOperationInput>) noexcept;
    signal_contracts::Status publish(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status publish_disabled(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameStamp&) noexcept;
    std::size_t output_rows(domain::PortId) const noexcept;
    SpectralOperationPlan plan_;
    std::variant<rhythm_reference_ports::PreparedPolarNovelty,rhythm_reference_ports::PreparedHistogramOnset,
        rhythm_reference_ports::PreparedSpectralWhitening,rhythm_reference_ports::PreparedOnsetMagnitude,
        rhythm_reference_ports::PreparedSpectralRmsFlux> kernel_;
    std::vector<float> values_;
    float result_=0;
};
}
