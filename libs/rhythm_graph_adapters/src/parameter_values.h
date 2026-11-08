// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace rhythm_graph_adapters::detail {
dataflow_runtime::NumericOperationNodeView definition_view(const dataflow_runtime::NumericOperationNode&) noexcept;
const domain::ProcessingParameterValue* parameter(const dataflow_runtime::NumericOperationNodeView&,std::string_view) noexcept;
bool scalar_parameter(const dataflow_runtime::NumericOperationNodeView&,std::string_view,float fallback,float&) noexcept;
bool integer_parameter(const dataflow_runtime::NumericOperationNodeView&,std::string_view,std::size_t fallback,std::size_t&) noexcept;
std::span<const float> array_parameter(const dataflow_runtime::NumericOperationNodeView&,std::string_view) noexcept;
inline bool scalar_parameter(const dataflow_runtime::NumericOperationNode& n,std::string_view key,float fallback,float& out) noexcept {
    return scalar_parameter(definition_view(n),key,fallback,out);
}
inline bool integer_parameter(const dataflow_runtime::NumericOperationNode& n,std::string_view key,std::size_t fallback,std::size_t& out) noexcept {
    return integer_parameter(definition_view(n),key,fallback,out);
}
inline std::span<const float> array_parameter(const dataflow_runtime::NumericOperationNode& n,std::string_view key) noexcept {
    return array_parameter(definition_view(n),key);
}
}
