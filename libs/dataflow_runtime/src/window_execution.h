#pragma once
#include "transform_execution.h"
namespace dataflow_runtime::detail {
TransformResult execute_window(const PlanNode&,State&,const DataFrameView&,TransformDelivery) noexcept;
}
