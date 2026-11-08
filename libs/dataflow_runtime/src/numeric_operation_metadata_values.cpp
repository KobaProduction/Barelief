#include "numeric_operation_requirements.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace dataflow_runtime::detail {
using namespace signal_contracts;
namespace {
bool input_value(const NumericOperationInvocation& call,domain::PortId port,NumericView& out,bool& available) noexcept {
    for(std::size_t i=0;i<call.input_buffers.size();++i){
        if(call.input_buffers[i].port!=port)continue;
        if(i>=call.inputs.size())return false;
        const auto* value=std::get_if<NumericView>(&call.inputs[i].payload);if(!value || !value->contract)return false;
        out=*value;available=call.input_buffers[i].status==Status::ok;return true;
    }
    return false;
}
bool output_value(const NumericOperationInvocation& call,domain::PortId port,NumericView& out,bool& available) noexcept {
    for(std::size_t i=0;i<call.output_buffers.size();++i){
        if(call.output_buffers[i].port!=port)continue;
        if(i>=call.outputs.size() || !call.outputs[i].contract)return false;
        out=call.outputs[i].view();available=call.output_buffers[i].status==Status::ok;return true;
    }
    return false;
}
}
bool metadata_value(const NumericOperationInvocation& call,domain::PortDirection direction,domain::PortId port,NumericView& out,bool& available) noexcept {
    available=false;
    if(direction==domain::PortDirection::Input)return input_value(call,port,out,available);
    return direction==domain::PortDirection::Output && output_value(call,port,out,available);
}
bool metadata_coordinate(std::span<const std::byte> bytes,std::size_t index,double& value) noexcept {
    if(index>=bytes.size()/8 || !bytes.data())return false;
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*8+byte]))<<(byte*8);
    value=std::bit_cast<double>(bits);return std::isfinite(value);
}
bool metadata_axis_payload(const NumericView& view,std::size_t axis) noexcept {
    if(!view.contract || axis>=2)return false;
    const auto bytes=view.coordinates.axes[axis];
    if(view.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return bytes.empty();
    if(view.actual[axis]>std::numeric_limits<std::size_t>::max()/8 || bytes.size()!=view.actual[axis]*8)return false;
    for(std::size_t i=0;i<view.actual[axis];++i){double value=0;if(!metadata_coordinate(bytes,i,value))return false;}
    return true;
}
namespace {
bool grid_values(const NumericView& view,const NumericOperationAxisRule& rule,NumericMetadataGrid grid) noexcept {
    const auto bytes=view.coordinates.axes[rule.axis];const auto count=view.actual[rule.axis];
    if(grid.any_origin && count && !metadata_coordinate(bytes,0,grid.origin))return false;
    for(std::size_t i=0;i<count;++i){
        double actual=0;const double expected=grid.origin+double(i)*grid.step;
        if(!metadata_coordinate(bytes,i,actual) || !metadata_close(actual,expected,grid.tolerance))return false;
    }
    return true;
}
bool increasing_values(const NumericView& view,const NumericOperationAxisRule& rule) noexcept {
    const auto& axis=view.contract->coordinates[rule.axis];
    if(!rule.require_increasing || axis.mode!=CoordinateMode::ExplicitFloat64LE || metadata_axis_ignored(axis,rule))return true;
    double previous=0;
    for(std::size_t i=0;i<view.actual[rule.axis];++i){
        double value=0;if(!metadata_coordinate(view.coordinates.axes[rule.axis],i,value) || (i && !(value>previous)))return false;
        previous=value;
    }
    return true;
}
bool axis_rule_values(const NumericOperationInvocation& call,const NumericOperationAxisRule& rule) noexcept {
    NumericView view;bool available=false;
    return metadata_value(call,rule.direction,rule.port,view,available) && (!available || metadata_rule_values(view,rule));
}
bool matching_axes(const NumericView& value,const NumericView& source,const NumericOperationAxisMatch& rule) noexcept {
    if(value.contract->coordinates[rule.axis]!=source.contract->coordinates[rule.source_axis])return false;
    if(rule.compare_tags && value.contract->shape.axes[rule.axis]!=source.contract->shape.axes[rule.source_axis])return false;
    if(!rule.compare_values)return true;
    return metadata_axis_payload(value,rule.axis) && metadata_axis_payload(source,rule.source_axis) &&
        std::ranges::equal(value.coordinates.axes[rule.axis],source.coordinates.axes[rule.source_axis]);
}
bool match_values(const NumericOperationInvocation& call,const NumericOperationAxisMatch& rule) noexcept {
    NumericView value,source;bool available=false,source_available=false;
    if(rule.axis>=2 || rule.source_axis>=2 || !metadata_value(call,rule.direction,rule.port,value,available))return false;
    if(!available)return true;
    if(!metadata_value(call,domain::PortDirection::Input,rule.source_port,source,source_available) || !source_available)return false;
    return matching_axes(value,source,rule);
}
}
bool metadata_rule_values(const NumericView& view,const NumericOperationAxisRule& rule) noexcept {
    if(!view.contract || !metadata_axis_valid(*view.contract,rule) || !metadata_axis_payload(view,rule.axis) || !increasing_values(view,rule))return false;
    const auto& axis=view.contract->coordinates[rule.axis];
    if(axis.mode!=CoordinateMode::ExplicitFloat64LE || rule.kind==NumericOperationAxisKind::Declaration || metadata_axis_ignored(axis,rule))return true;
    NumericMetadataGrid grid;return metadata_grid(axis,rule,grid) && grid_values(view,rule,grid);
}
Status numeric_operation_metadata_values(const NumericOperationInvocation& call,const NumericOperationPolicy& policy,domain::PortDirection direction) noexcept {
    if(direction!=domain::PortDirection::Input && direction!=domain::PortDirection::Output)return Status::invalid_argument;
    for(const auto& rule:policy.axes)if(rule.direction==direction && !axis_rule_values(call,rule))return Status::incompatible_contract;
    for(const auto& rule:policy.axis_matches)if(rule.direction==direction && !match_values(call,rule))return Status::incompatible_contract;
    return Status::ok;
}
}
