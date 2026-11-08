#pragma once
// Composition/runtime boundary only. Numerical factories use numeric_operation.h.
#include <dataflow_runtime/numeric_operation.h>
namespace dataflow_runtime {
struct NumericRuntimeProfile;
NumericOperationNodeView numeric_operation_node_view(const domain::ResolvedProcessingNode&) noexcept;
NumericOperationNode numeric_operation_node(const domain::ConfiguredProcessingNode&);
NumericOperationProfile numeric_operation_profile(const NumericRuntimeProfile&) noexcept;
} // namespace dataflow_runtime
