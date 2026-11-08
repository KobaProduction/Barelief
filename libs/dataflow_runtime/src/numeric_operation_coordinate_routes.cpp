#include "numeric_operation_requirements.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dataflow_runtime::detail {
using namespace signal_contracts;
namespace {
bool output_index(const NumericOperationInvocation& call,domain::PortId port,std::size_t& index) noexcept {
    for(std::size_t i=0;i<call.output_buffers.size();++i)if(call.output_buffers[i].port==port){index=i;return true;}
    return false;
}
bool output_axis(const NumericOperationInvocation& call,std::size_t index,std::size_t axis) noexcept {
    return axis<2 && index<call.outputs.size() && index<call.output_buffers.size() && index<call.coordinate_storage.size() &&
        index<call.coordinate_counts.size() && call.outputs[index].contract;
}
Status copy_axis(NumericOperationInvocation& call,const NumericOperationCoordinateRoute& route,std::size_t index,const NumericView& source) noexcept {
    const auto axis=route.output_axis;const auto bytes=source.coordinates.axes[route.input_axis];
    const auto count=call.outputs[index].actual[axis];const auto storage=call.coordinate_storage[index][axis];
    if(source.actual[route.input_axis]!=count || !metadata_axis_payload(source,route.input_axis))return Status::incompatible_contract;
    if(count>storage.size()/8 || count>std::numeric_limits<std::size_t>::max()/8)return Status::buffer_too_small;
    if(call.coordinate_counts[index][axis]){
        if(call.coordinate_counts[index][axis]!=count || !std::ranges::equal(bytes,storage.first(count*8)))return Status::incompatible_contract;
        return Status::ok;
    }
    std::copy(bytes.begin(),bytes.end(),storage.begin());call.coordinate_counts[index][axis]=count;return Status::ok;
}
Status route_axis(NumericOperationInvocation& call,const NumericOperationCoordinateRoute& route) noexcept {
    std::size_t index=0;if(!output_index(call,route.output,index) || !output_axis(call,index,route.output_axis) || route.input_axis>=2)return Status::incompatible_contract;
    if(call.output_buffers[index].status!=Status::ok)return Status::ok;
    NumericView source;bool available=false;
    if(!metadata_value(call,domain::PortDirection::Input,route.input,source,available) || !available)return Status::incompatible_contract;
    const auto& target=call.outputs[index].contract->coordinates[route.output_axis];
    if(target!=source.contract->coordinates[route.input_axis])return Status::incompatible_contract;
    if(target.mode!=CoordinateMode::ExplicitFloat64LE)return call.coordinate_counts[index][route.output_axis]?Status::incompatible_contract:Status::ok;
    return copy_axis(call,route,index,source);
}
bool routed_axis(const NumericOperationPolicy& policy,domain::PortId port,std::size_t axis) noexcept {
    return std::ranges::any_of(policy.coordinate_routes,[&](const auto& route){return route.output==port && route.output_axis==axis;});
}
bool write_grid(std::span<std::byte> storage,std::size_t count,const NumericMetadataGrid& grid) noexcept {
    for(std::size_t i=0;i<count;++i){
        const double value=grid.origin+double(i)*grid.step;if(!std::isfinite(value))return false;
        const auto bits=std::bit_cast<std::uint64_t>(value);
        for(std::size_t byte=0;byte<8;++byte)storage[i*8+byte]=std::byte((bits>>(byte*8))&255);
    }
    return true;
}
Status generate_grid(NumericOperationInvocation& call,std::size_t index,const NumericOperationAxisRule& rule) noexcept {
    const auto& contract=*call.outputs[index].contract;const auto& axis=contract.coordinates[rule.axis];
    if(!metadata_axis_valid(contract,rule))return Status::incompatible_contract;
    if(axis.mode!=CoordinateMode::ExplicitFloat64LE || metadata_axis_ignored(axis,rule))return Status::ok;
    const auto count=call.outputs[index].actual[rule.axis];const auto storage=call.coordinate_storage[index][rule.axis];
    if(count>storage.size()/8)return Status::buffer_too_small;
    NumericMetadataGrid grid;if(!metadata_grid(axis,rule,grid) || !write_grid(storage,count,grid))return Status::incompatible_contract;
    call.coordinate_counts[index][rule.axis]=count;return Status::ok;
}
Status generate_axis(NumericOperationInvocation& call,const NumericOperationPolicy& policy,const NumericOperationAxisRule& rule) noexcept {
    if(rule.direction!=domain::PortDirection::Output || rule.kind==NumericOperationAxisKind::Declaration)return Status::ok;
    if(rule.kind!=NumericOperationAxisKind::Index && rule.kind!=NumericOperationAxisKind::Uniform)return Status::incompatible_contract;
    std::size_t index=0;if(!output_index(call,rule.port,index) || !output_axis(call,index,rule.axis))return Status::incompatible_contract;
    if(call.output_buffers[index].status!=Status::ok || call.coordinate_counts[index][rule.axis] || routed_axis(policy,rule.port,rule.axis))return Status::ok;
    return generate_grid(call,index,rule);
}
}
Status numeric_operation_route_coordinates(NumericOperationInvocation& call,const NumericOperationPolicy& policy) noexcept {
    for(const auto& route:policy.coordinate_routes){const auto status=route_axis(call,route);if(status!=Status::ok)return status;}
    for(const auto& rule:policy.axes){const auto status=generate_axis(call,policy,rule);if(status!=Status::ok)return status;}
    return Status::ok;
}
}
