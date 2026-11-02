#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <string_view>

namespace dsp_graph_adapters::detail {
bool common_operation_kind(std::string_view) noexcept;
signal_contracts::Status describe_common_operation(const domain::ResolvedProcessingNode&,
    const dataflow_runtime::NumericOperationFactory*,std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor>&);
domain::ProcessingSupport assess_common_operation(const domain::ResolvedProcessingNode&,
    const dataflow_runtime::NumericRuntimeProfile&) noexcept;
signal_contracts::Status prepare_common_operation(std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor>,
    const dataflow_runtime::NumericRuntimeProfile&,std::unique_ptr<dataflow_runtime::PreparedNumericOperation>&);
}
