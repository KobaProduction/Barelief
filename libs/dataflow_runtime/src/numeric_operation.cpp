#include <dataflow_runtime/numeric_operation.h>
#include <algorithm>
#include <limits>
#include <utility>

namespace dataflow_runtime {
using namespace signal_contracts;
bool numeric_operation_add_bytes(std::size_t& total,std::size_t value) noexcept {
    if(value>std::numeric_limits<std::size_t>::max()-total)return false;total+=value;return true;
}
bool numeric_operation_mul_bytes(std::size_t count,std::size_t width,std::size_t& output) noexcept {
    if(width && count>std::numeric_limits<std::size_t>::max()/width)return false;output=count*width;return true;
}
namespace {
NumericOperationNode own_node(const NumericOperationNodeView& source) {
    NumericOperationNode node;node.id=source.id;node.kind=source.kind;node.enabled=source.enabled;node.parameters=*source.parameters;
    node.inputs.reserve(source.inputs.size());node.outputs.reserve(source.outputs.size());
    for(const auto port:source.inputs)node.inputs.push_back(port);
    for(const auto port:source.outputs)node.outputs.push_back(port);
    return node;
}
}
Status own_numeric_operation_descriptor(const NumericOperationNodeView& source,NumericOperationPolicy policy,
    std::shared_ptr<const NumericOperationDescriptor>& output,const NumericOperationFactory* identity) {
    if(!source.parameters || source.kind.empty())return Status::invalid_argument;
    auto candidate=std::make_shared<NumericOperationDescriptor>();candidate->node=own_node(source);
    candidate->policy=std::move(policy);candidate->factory_identity=identity;output=std::move(candidate);return Status::ok;
}
namespace {
bool add_array(std::size_t& bytes,std::size_t count,std::size_t width) noexcept {
    std::size_t extra=0;return numeric_operation_mul_bytes(count,width,extra) && numeric_operation_add_bytes(bytes,extra);
}
bool definition_bytes(const NumericOperationNodeView& source,const domain::ProcessingAdmissionLimits& limits,
                      std::size_t kind_capacity,std::size_t inputs,std::size_t outputs,std::size_t& bytes) noexcept {
    domain::ResolvedProcessingNode settings;settings.kind=source.kind;settings.parameters=source.parameters;
    if(!domain::processing_node_required_bytes(settings,limits,bytes,false))return false;
    const auto removed=sizeof(domain::ConfiguredProcessingNode)+std::max(source.kind.size(),sizeof(std::string));
    if(bytes<removed)return false;bytes-=removed;
    return numeric_operation_add_bytes(bytes,sizeof(NumericOperationDescriptor)) && numeric_operation_add_bytes(bytes,kind_capacity) &&
        add_array(bytes,inputs,sizeof(NumericOperationPort)) && add_array(bytes,outputs,sizeof(NumericOperationPort));
}
bool actual_policy_bytes(const NumericOperationPolicy& policy,std::size_t& bytes) noexcept {
    if(!add_array(bytes,policy.inputs.capacity(),sizeof(NumericOperationInputRule)) ||
       !add_array(bytes,policy.output_values.capacity(),sizeof(ValueDomain)) ||
       !add_array(bytes,policy.clocks.capacity(),sizeof(NumericOperationClockRule)) ||
       !add_array(bytes,policy.axes.capacity(),sizeof(NumericOperationAxisRule)) ||
       !add_array(bytes,policy.meanings.capacity(),sizeof(NumericOperationMeaningRule)) ||
       !add_array(bytes,policy.axis_matches.capacity(),sizeof(NumericOperationAxisMatch)) ||
       !add_array(bytes,policy.coordinate_routes.capacity(),sizeof(NumericOperationCoordinateRoute)))return false;
    for(const auto& meaning:policy.meanings)
        if(!add_array(bytes,meaning.quantities.capacity(),sizeof(Quantity)) || !add_array(bytes,meaning.units.capacity(),sizeof(NumericUnit)))return false;
    return true;
}
bool policy_bound(std::size_t ports,std::size_t& bytes) noexcept {
    // Two physical axes per port, including vector growth and paired matches.
    constexpr auto units=std::size_t(NumericUnit::Decibels)+1;
    constexpr auto quantities=std::size_t(Quantity::BroadbandBinPowerFlux)+1;
    constexpr std::size_t width=2*sizeof(NumericOperationInputRule)+2*sizeof(ValueDomain)+
        2*sizeof(NumericOperationClockRule)+4*sizeof(NumericOperationAxisRule)+2*sizeof(NumericOperationMeaningRule)+
        4*sizeof(NumericOperationAxisMatch)+4*sizeof(NumericOperationCoordinateRoute)+4*(units+quantities)*sizeof(std::uint32_t);
    return add_array(bytes,ports,width);
}
}
bool numeric_operation_descriptor_required_bytes(const NumericOperationNodeView& source,
    const domain::ProcessingAdmissionLimits& limits,std::size_t& output) noexcept {
    std::size_t bytes=0,ports=source.inputs.size();
    if(!numeric_operation_add_bytes(ports,source.outputs.size()) ||
       !definition_bytes(source,limits,std::max(source.kind.size(),sizeof(std::string)),source.inputs.size(),source.outputs.size(),bytes) ||
       !policy_bound(ports,bytes))return false;
    output=bytes;return true;
}
bool numeric_operation_descriptor_owned_bytes(const NumericOperationDescriptor& source,
    const domain::ProcessingAdmissionLimits& limits,std::size_t& output) noexcept {
    std::size_t bytes=0;
    if(!definition_bytes(source.node,limits,source.node.kind.capacity(),source.node.inputs.capacity(),source.node.outputs.capacity(),bytes) ||
       !actual_policy_bytes(source.policy,bytes))return false;
    output=bytes;return true;
}
} // namespace dataflow_runtime
