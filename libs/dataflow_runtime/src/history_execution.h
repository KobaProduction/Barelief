#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
struct HistoryAppendResult {Status status=Status::ok;bool reject=false;};
HistoryAppendResult append_history(const PlanNode&,State&,const DataFrameView&,const FrameLimits&) noexcept;
void select_filled_history(const PlanNode&,const State&,NumericView&) noexcept;
}
