#pragma once
#include <domain/graph_definition.h>
#include <algorithm>
#include <array>
namespace domain::detail {
using C=ParameterClassification;using V=ParameterValueKind;
struct ParameterSet {NodeKind kind;std::span<const ParameterDescriptor> fields;};
inline std::span<const ParameterDescriptor> find_parameters(std::span<const ParameterSet> sets,NodeKind kind) noexcept {
    for(const auto& set:sets)if(set.kind==kind)return set.fields;
    return {};
}
inline constexpr ParameterDescriptor gain[]{{"gain",C::Processing,V::Number,0,100,1,{}}};
inline constexpr std::array fill_parameters{
    ParameterDescriptor{"value",C::Processing,V::Number,-1e30,1e30,1,{}},
    ParameterDescriptor{"size",C::Processing,V::Integer,1,4294967295.,2048,{}},
    ParameterDescriptor{"values",C::Processing,V::NumericArray,0,32768,0,{}}};
std::span<const ParameterDescriptor> numeric_parameter_descriptors(NodeKind) noexcept;
std::span<const ParameterDescriptor> signal_parameter_descriptors(NodeKind) noexcept;
std::span<const ParameterDescriptor> music_parameter_descriptors(NodeKind) noexcept;
std::span<const ParameterDescriptor> routing_parameter_descriptors(NodeKind) noexcept;
std::span<const ParameterDescriptor> adapter_parameter_descriptors(NodeKind) noexcept;
std::span<const ParameterDescriptor> view_parameter_descriptors(NodeKind) noexcept;
}
