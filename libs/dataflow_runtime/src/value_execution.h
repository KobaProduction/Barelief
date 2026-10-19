#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
// Value operations use prepared buffers and publish an output only on success.
Status execute_matrix_row(const PlanNode&,State&,const DataFrameView&,const FrameLimits&) noexcept;
Status execute_array_fill(const PlanNode&,State&,const DataFrameView&,const FrameLimits&) noexcept;
Status execute_metadata(const PlanNode&,State&,const DataFrameView&,const FrameLimits&) noexcept;
Status execute_extract(const PlanNode&,State&,const DataFrameView&,const FrameLimits&) noexcept;

Status execute_spectrum_peak(const PlanNode&,State&,const DataFrameView&,const FrameLimits&) noexcept;
}
