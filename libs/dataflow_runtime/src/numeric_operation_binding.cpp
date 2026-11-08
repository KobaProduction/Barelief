#include "numeric_operation_bridge.h"
#include <dataflow_runtime/runtime.h>

namespace dataflow_runtime {
namespace {
NumericOperationPort read_restricted(const void* data,std::size_t index) noexcept {
    return static_cast<const NumericOperationPort*>(data)[index];
}
NumericOperationPort read_binding(const void* data,std::size_t index) noexcept {
    const auto& port=static_cast<const domain::ProcessingPort*>(data)[index];
    const auto value=port.contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&value);
    return {port.id,port.direction,numeric?detail::NumericOperationBindingAccess::storage(*numeric):signal_contracts::NumericBufferContract{}};
}
}
NumericOperationPorts::NumericOperationPorts(std::span<const NumericOperationPort> ports) noexcept
    :data_(ports.data()),size_(ports.size()),read_(read_restricted){}
NumericOperationNodeView::NumericOperationNodeView(const NumericOperationNode& node) noexcept
    :id(node.id),kind(node.kind),inputs(node.inputs),outputs(node.outputs),parameters(&node.parameters),enabled(node.enabled){}
NumericOperationNodeView numeric_operation_node_view(const domain::ResolvedProcessingNode& node) noexcept {
    NumericOperationNodeView result;result.id=node.id;result.kind=node.kind;result.parameters=node.parameters;result.enabled=node.enabled;
    result.inputs=detail::NumericOperationBindingAccess::ports(node.inputs);result.outputs=detail::NumericOperationBindingAccess::ports(node.outputs);return result;
}
NumericOperationNode numeric_operation_node(const domain::ConfiguredProcessingNode& node) {
    NumericOperationNode result;result.id=node.id;result.kind=node.kind;result.parameters=node.parameters;result.enabled=node.enabled;
    result.inputs.reserve(node.inputs.size());result.outputs.reserve(node.outputs.size());
    for(const auto& port:node.inputs)result.inputs.push_back(read_binding(&port,0));
    for(const auto& port:node.outputs)result.outputs.push_back(read_binding(&port,0));
    return result;
}
NumericOperationProfile numeric_operation_profile(const NumericRuntimeProfile& profile) noexcept {
    return {profile.admission,profile.resources,profile.kernels,profile.capabilities};
}
signal_contracts::Status resolve_numeric_operation_parameter(const NumericOperationNodeView& node,
    std::string_view key,domain::ProcessingParameterView& output) noexcept {
    domain::ResolvedProcessingNode settings;settings.kind=node.kind;settings.parameters=node.parameters;
    return domain::resolve_processing_parameter(settings,key,output);
}
namespace detail {
signal_contracts::NumericBufferContract NumericOperationBindingAccess::storage(const signal_contracts::NumericContract& value) noexcept {
    return {value.dtype,{value.shape.rank,value.shape.extents},value.representation,value.channel_order.size()};
}
NumericOperationPorts NumericOperationBindingAccess::ports(std::span<const domain::ProcessingPort> ports) noexcept {
    NumericOperationPorts result;result.data_=ports.data();result.size_=ports.size();result.read_=read_binding;return result;
}
}
} // namespace dataflow_runtime
