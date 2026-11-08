// SPDX-License-Identifier: AGPL-3.0-or-later
#include "parameter_values.h"
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_graph_adapters::detail {
dataflow_runtime::NumericOperationNodeView definition_view(const dataflow_runtime::NumericOperationNode& node) noexcept {return dataflow_runtime::NumericOperationNodeView(node);}
const domain::ProcessingParameterValue* parameter(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key) noexcept {
    const auto* values=node.parameters?std::get_if<domain::TransformProcessingParameters>(node.parameters):nullptr;
    if(!values)return nullptr;
    for(const auto& item:values->values)if(item.key==key)return &item.value;
    return nullptr;
}
bool scalar_parameter(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key,float fallback,float& value) noexcept {
    const auto* found=parameter(node,key);
    if(!found){value=fallback;return true;}
    const auto* literal=std::get_if<signal_contracts::NumericLiteral>(found);
    if(!literal)return false;
    value=std::visit([](auto number){return float(number);},*literal);return std::isfinite(value);
}
bool integer_parameter(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key,std::size_t fallback,std::size_t& value) noexcept {
    const auto* found=parameter(node,key);
    if(!found){value=fallback;return true;}
    const auto* literal=std::get_if<signal_contracts::NumericLiteral>(found);
    if(!literal)return false;
    return std::visit([&](auto number){
        using T=decltype(number);
        if constexpr(!std::is_integral_v<T>)return false;
        else {if(!std::in_range<std::size_t>(number))return false;value=std::size_t(number);return true;}
    },*literal);
}
std::span<const float> array_parameter(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key) noexcept {
    const auto* found=parameter(node,key);
    const auto* array=found?std::get_if<domain::NumericArray>(found):nullptr;
    return array?std::span<const float>(array->values):std::span<const float>{};
}
}
