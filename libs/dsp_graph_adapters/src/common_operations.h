#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <string_view>

namespace dsp_graph_adapters::detail {
bool common_operation_kind(std::string_view) noexcept;
signal_contracts::Status describe_common_operation(const dataflow_runtime::NumericOperationNodeView&,
    const dataflow_runtime::NumericOperationFactory*,std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor>&);
domain::ProcessingSupport assess_common_operation(const dataflow_runtime::NumericOperationNodeView&,
    const dataflow_runtime::NumericOperationProfile&) noexcept;
signal_contracts::Status prepare_common_operation(std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor>,
    const dataflow_runtime::NumericOperationProfile&,std::unique_ptr<dataflow_runtime::PreparedNumericOperation>&);
}
