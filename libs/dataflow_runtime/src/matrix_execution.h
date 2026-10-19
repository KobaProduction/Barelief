#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
// Processes an available borrowed numeric matrix using prepared storage and
// kernels. Publishes state.numeric_frames[0] only on success; never allocates.
Status execute_matrix(const PlanNode&, State&, const DataFrameView&, const FrameLimits&, Ratio execution_interval) noexcept;
}
