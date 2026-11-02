#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
constexpr bool matrix_processing(domain::ProcessingOperation operation) noexcept {
    using domain::ProcessingOperation;
    return operation==ProcessingOperation::MatrixTranspose || operation==ProcessingOperation::MatrixWindowFunction ||
        operation==ProcessingOperation::MatrixFFT || operation==ProcessingOperation::MatrixCenter ||
        operation==ProcessingOperation::MatrixAutocorrelation;
}
// Processes an available borrowed numeric matrix using prepared storage and
// kernels. Publishes prepared outputs only on success; never allocates.
Status execute_matrix(const PlanNode&, State&, const DataFrameView&, const FrameLimits&, Ratio execution_interval) noexcept;
}
