#pragma once
#include "prepared_node.h"
namespace dataflow_runtime::detail {
struct State;
Status prepare_axis_extraction(PlanNode&,const ResolvedProcessingNode&);
Status execute_axis_extraction(const PlanNode&,State&,const NumericView&,MutableNumericView&,const FrameLimits&) noexcept;
}
