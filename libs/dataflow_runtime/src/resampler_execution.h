#pragma once
#include "transform_execution.h"
namespace dataflow_runtime::detail {
TransformResult execute_resampler(const PlanNode&,State&,const DataFrameView&,TransformDelivery) noexcept;
}
