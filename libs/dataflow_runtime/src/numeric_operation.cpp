#include <dataflow_runtime/numeric_operation.h>
#include <cmath>
#include <cstring>
#include <utility>
#include <limits>

namespace dataflow_runtime {
using namespace signal_contracts;
Status own_numeric_operation_descriptor(const domain::ResolvedProcessingNode& source,NumericOperationPolicy policy,
    std::shared_ptr<const NumericOperationDescriptor>& output,const NumericOperationFactory* identity) {
    if(!source.parameters || source.kind.empty())return Status::invalid_argument;
    auto candidate=std::make_shared<NumericOperationDescriptor>();
    candidate->node.id=source.id;candidate->node.kind=std::string(source.kind);candidate->node.enabled=source.enabled;
    candidate->node.parameters=*source.parameters;
    candidate->node.inputs.assign(source.inputs.begin(),source.inputs.end());
    candidate->node.outputs.assign(source.outputs.begin(),source.outputs.end());
    candidate->policy=std::move(policy);candidate->factory_identity=identity;output=std::move(candidate);return Status::ok;
}
bool numeric_operation_add_bytes(std::size_t& total,std::size_t value) noexcept {
    if(value>std::numeric_limits<std::size_t>::max()-total)return false;
    total+=value;return true;
}
bool numeric_operation_mul_bytes(std::size_t count,std::size_t width,std::size_t& output) noexcept {
    if(width && count>std::numeric_limits<std::size_t>::max()/width)return false;
    output=count*width;return true;
}
namespace {
bool descriptor_bytes(std::size_t node_bytes,std::size_t inputs,std::size_t output_domains,std::size_t& output) noexcept {
    if(node_bytes<sizeof(domain::ConfiguredProcessingNode))return false;
    std::size_t bytes=node_bytes-sizeof(domain::ConfiguredProcessingNode),extra=0;
    if(!numeric_operation_add_bytes(bytes,sizeof(NumericOperationDescriptor)) ||
       !numeric_operation_mul_bytes(inputs,sizeof(NumericOperationInputRule),extra) || !numeric_operation_add_bytes(bytes,extra) ||
       !numeric_operation_mul_bytes(output_domains,sizeof(ValueDomain),extra) || !numeric_operation_add_bytes(bytes,extra))return false;
    output=bytes;return true;
}
}
bool numeric_operation_descriptor_required_bytes(const domain::ResolvedProcessingNode& source,const domain::ProcessingAdmissionLimits& limits,std::size_t& output) noexcept {
    std::size_t bytes=0;
    return domain::processing_node_required_bytes(source,limits,bytes,false) && descriptor_bytes(bytes,source.inputs.size(),source.outputs.size(),output);
}
bool numeric_operation_descriptor_owned_bytes(const NumericOperationDescriptor& source,const domain::ProcessingAdmissionLimits& limits,std::size_t& output) noexcept {
    std::size_t bytes=0;
    return domain::processing_node_owned_bytes(source.node,limits,bytes,false) && descriptor_bytes(bytes,source.policy.inputs.capacity(),source.policy.output_values.capacity(),output);
}
Status numeric_operation_coordinate(NumericOperationOutput& output,std::size_t axis,std::size_t index,double value) noexcept {
    if(axis>=2 || !std::isfinite(value))return Status::invalid_argument;
    auto bytes=output.writable_coordinates[axis];
    if(index>=bytes.size()/sizeof(double))return Status::buffer_too_small;
    std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
    for(std::size_t byte=0;byte<sizeof(bits);++byte)bytes[index*sizeof(bits)+byte]=std::byte((bits>>(8*byte))&255);
    return Status::ok;
}
Status numeric_operation_axis(NumericOperationOutput& output,std::size_t axis,std::size_t count) noexcept {
    if(axis>=2 || !output.value.contract || output.value.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return Status::invalid_argument;
    const auto bytes=output.writable_coordinates[axis];
    if(count>bytes.size()/sizeof(double) || count!=output.value.actual[axis])return Status::buffer_too_small;
    output.value.coordinates.axes[axis]=bytes.first(count*sizeof(double));return Status::ok;
}
} // namespace dataflow_runtime
