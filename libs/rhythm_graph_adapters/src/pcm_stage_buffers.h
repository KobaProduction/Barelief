// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace rhythm_graph_adapters::detail {
bool pcm_stage_buffers_disjoint(std::span<const dataflow_runtime::NumericOperationInput>,
    std::span<const dataflow_runtime::NumericOperationOutput>) noexcept;
}
