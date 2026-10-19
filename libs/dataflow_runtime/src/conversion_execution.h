#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
struct ConversionHistoryOrigin {NodeId node=-1;std::uint64_t ordinal=0;std::size_t progress=0;};
struct ConversionExecutionResult {Status status=Status::ok;bool reject=false;};
ConversionExecutionResult execute_conversion(const PlanNode&,State&,const DataFrameView&,const FrameLimits&,std::optional<ConversionHistoryOrigin>) noexcept;
}
