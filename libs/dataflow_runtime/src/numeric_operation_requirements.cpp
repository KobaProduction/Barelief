#include "numeric_operation_requirements.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dataflow_runtime::detail {
using namespace signal_contracts;
namespace {
bool valid_direction(domain::PortDirection direction) noexcept {
    return direction==domain::PortDirection::Input || direction==domain::PortDirection::Output;
}
bool valid_ports(std::span<const domain::ProcessingPort> ports) noexcept {
    constexpr auto maximum=std::numeric_limits<std::size_t>::max();
    constexpr FrameLimits limits{maximum,maximum,maximum,maximum,maximum,maximum};
    for(const auto& port:ports){
        const auto value=port.contract.view();
        if(!std::holds_alternative<NumericContract>(value) || validate_contract(value,limits)!=Status::ok)return false;
    }
    return true;
}
bool same_clock(const ClockContract& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick_seconds==b.tick_seconds && a.sample_rate==b.sample_rate;
}
bool clock_rule(const domain::ResolvedProcessingNode& node,const NumericOperationClockRule& rule) noexcept {
    NumericContract value,source;if(!metadata_contract(node,rule.direction,rule.port,value))return false;
    if(rule.kind==NumericOperationClockKind::Unclocked)
        return value.clock.domain.empty() && !value.clock.sample_rate && value.clock.tick_seconds==Ratio{1,1};
    if(rule.kind==NumericOperationClockKind::MatchInput){
        if(!metadata_contract(node,domain::PortDirection::Input,rule.source_port,source))return false;
        const bool unclocked=value.clock.domain.empty() && !value.clock.sample_rate && value.clock.tick_seconds==Ratio{1,1};
        return same_clock(value.clock,source.clock) || (rule.allow_unknown && unclocked);
    }
    if(rule.kind!=NumericOperationClockKind::SampleRate || !std::isfinite(rule.sample_rate) || rule.sample_rate<=0)return false;
    return value.clock.sample_rate?double(value.clock.sample_rate)==rule.sample_rate:rule.allow_unknown;
}
bool meaning_rule(const domain::ResolvedProcessingNode& node,const NumericOperationMeaningRule& rule) noexcept {
    NumericContract value,source;if(!metadata_contract(node,rule.direction,rule.port,value))return false;
    if(!rule.quantities.empty() && std::ranges::find(rule.quantities,value.quantity)==rule.quantities.end())return false;
    if(!rule.units.empty() && std::ranges::find(rule.units,value.unit)==rule.units.end())return false;
    return !rule.same_unit_as_input || (metadata_contract(node,domain::PortDirection::Input,*rule.same_unit_as_input,source) && source.unit==value.unit);
}
bool axis_match(const domain::ResolvedProcessingNode& node,const NumericOperationAxisMatch& rule) noexcept {
    NumericContract value,source;
    if(rule.axis>=2 || rule.source_axis>=2 || !metadata_contract(node,rule.direction,rule.port,value) ||
       !metadata_contract(node,domain::PortDirection::Input,rule.source_port,source))return false;
    return value.coordinates[rule.axis]==source.coordinates[rule.source_axis] &&
        (!rule.compare_tags || value.shape.axes[rule.axis]==source.shape.axes[rule.source_axis]);
}
bool coordinate_route(const domain::ResolvedProcessingNode& node,const NumericOperationCoordinateRoute& rule) noexcept {
    NumericContract input,output;
    return rule.input_axis<2 && rule.output_axis<2 && metadata_contract(node,domain::PortDirection::Input,rule.input,input) &&
        metadata_contract(node,domain::PortDirection::Output,rule.output,output) && input.coordinates[rule.input_axis]==output.coordinates[rule.output_axis];
}
bool valid_routes(const domain::ResolvedProcessingNode& node,const NumericOperationPolicy& policy) noexcept {
    for(std::size_t i=0;i<policy.coordinate_routes.size();++i){
        const auto& route=policy.coordinate_routes[i];if(!coordinate_route(node,route))return false;
        for(std::size_t j=0;j<i;++j)if(route.output==policy.coordinate_routes[j].output && route.output_axis==policy.coordinate_routes[j].output_axis)return false;
    }
    return true;
}
bool contract_requirements(const domain::ResolvedProcessingNode& node,const NumericOperationPolicy& policy) noexcept {
    for(const auto& rule:policy.clocks)if(!clock_rule(node,rule))return false;
    for(const auto& rule:policy.axes){
        NumericContract value;if(!metadata_contract(node,rule.direction,rule.port,value) || !metadata_axis_valid(value,rule))return false;
    }
    return true;
}
bool relational_requirements(const domain::ResolvedProcessingNode& node,const NumericOperationPolicy& policy) noexcept {
    for(const auto& rule:policy.meanings)if(!meaning_rule(node,rule))return false;
    for(const auto& rule:policy.axis_matches)if(!axis_match(node,rule))return false;
    return valid_routes(node,policy);
}
}
bool metadata_contract(const domain::ResolvedProcessingNode& node,domain::PortDirection direction,domain::PortId id,NumericContract& out) noexcept {
    if(!valid_direction(direction))return false;
    for(const auto& port:direction==domain::PortDirection::Input?node.inputs:node.outputs){
        if(port.id!=id)continue;
        const auto value=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&value);
        if(!numeric)return false;
        out=*numeric;return true;
    }
    return false;
}
bool numeric_operation_metadata_valid(const domain::ResolvedProcessingNode& node,const NumericOperationPolicy& policy) noexcept {
    if(!valid_ports(node.inputs) || !valid_ports(node.outputs))return false;
    NumericContract stream;
    if(policy.stream_input && !metadata_contract(node,domain::PortDirection::Input,*policy.stream_input,stream))return false;
    return contract_requirements(node,policy) && relational_requirements(node,policy);
}
}
