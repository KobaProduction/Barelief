// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adapter_operation.h"
#include <rhythm_reference_ports/polar_novelty.h>
#include <rhythm_reference_ports/histogram_onsets.h>
#include <rhythm_reference_ports/onset_magnitude.h>

namespace rhythm_graph_adapters::detail {
inline constexpr domain::PortId spectral_control_port=65500;
enum class SpectralOperationKind {Polar,Histogram,Whitening,Magnitude,Rms};
struct SpectralOperationPlan {
    SpectralOperationKind kind=SpectralOperationKind::Polar;
    std::size_t bins=0,kernel_bytes=0,output_buffer_bytes=0;
    rhythm_reference_ports::PolarNovelty polar=rhythm_reference_ports::PolarNovelty::Complex;
    rhythm_reference_ports::HistogramOnsetConfig histogram;
    rhythm_reference_ports::OnsetMagnitudeConfig magnitude;
    bool phase_input() const noexcept {
        return kind==SpectralOperationKind::Polar || (kind==SpectralOperationKind::Histogram &&
            histogram.method!=rhythm_reference_ports::HistogramOnsetMethod::SpectralDifference);
    }
};
bool spectral_operation_kind(std::string_view) noexcept;
bool spectral_port(const domain::ResolvedProcessingNode&,bool input,domain::PortId,signal_contracts::NumericContract&) noexcept;
bool spectral_same_clock(const signal_contracts::ClockContract&,const signal_contracts::ClockContract&) noexcept;
bool plan_spectral_operation(const domain::ResolvedProcessingNode&,SpectralOperationPlan&) noexcept;
bool spectral_output_contracts(const domain::ResolvedProcessingNode&,const SpectralOperationPlan&) noexcept;
domain::ProcessingSupport assess_spectral_operation(const domain::ResolvedProcessingNode&,const dataflow_runtime::NumericRuntimeProfile&) noexcept;
dataflow_runtime::NumericOperationPolicy spectral_operation_policy(const domain::ResolvedProcessingNode&);
std::unique_ptr<AdapterOperation> prepare_spectral_operation(const domain::ConfiguredProcessingNode&);
}
