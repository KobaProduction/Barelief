// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <dsp_kernels/numeric_rows.h>

namespace rhythm_graph_adapters::detail {
dsp_kernels::FloatMatrixSource sequence_source_rows(const dataflow_runtime::NumericOperationInput&) noexcept;
dsp_kernels::FloatMatrixDestination sequence_source_writer(dataflow_runtime::NumericOperationOutput&) noexcept;
signal_contracts::Status sequence_source_flags(dataflow_runtime::NumericOperationOutput&,std::span<const std::uint8_t>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
signal_contracts::Status sequence_source_centers(dataflow_runtime::NumericOperationOutput&,std::span<const double>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
}
