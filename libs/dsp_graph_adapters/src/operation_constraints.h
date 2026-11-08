#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace dsp_graph_adapters::detail {
// Explicit settings become runtime requirements; actual metadata never enters adapters.
bool common_constraints(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
void sequence_constraints(const dataflow_runtime::NumericOperationNodeView&,dataflow_runtime::NumericOperationPolicy&);
}
