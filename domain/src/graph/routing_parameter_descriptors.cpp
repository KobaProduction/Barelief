#include "parameter_sets.h"
#include <domain/graph/processing_model.h>
#include <limits>
namespace domain::detail {
namespace {
std::optional<NumericParameterBounds> selected_route_bounds(const ConfiguredProcessingNode& node) noexcept {
    const auto* route=std::get_if<RouteProcessingParameters>(&node.parameters);
    if(!route || route->outputs<min_route_outputs || route->outputs>max_route_outputs)return {};
    return NumericParameterBounds{0,double(route->outputs-1)};
}
constexpr ParameterDescriptor selector[]{
        {"selected_input",C::Processing,V::Integer,0,65535,0,{}},
        {"channels_per_stream",C::Processing,V::Integer,1,max_route_channels,1,{}}};
constexpr ParameterDescriptor clock[]{
        {"sample_rate",C::Processing,V::Integer,min_sample_clock_rate,max_sample_clock_rate,48000,{}},
        {"domain",C::Processing,V::String,0,0,0,"local"}};
constexpr ParameterDescriptor mixer[]{
        {"fifo_milliseconds",C::Processing,V::Integer,min_mixer_fifo_ms,max_mixer_fifo_ms,100,{}}};
constexpr ParameterDescriptor split[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},{"outputs",C::Processing,V::Integer,min_route_outputs,max_route_outputs,min_route_outputs,{}}};
constexpr ParameterDescriptor route[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},{"outputs",C::Processing,V::Integer,min_route_outputs,max_route_outputs,min_route_outputs,{}},
        {"selected",C::Processing,V::Integer,0,max_route_outputs-1,0,{}, {},selected_route_bounds},
        {"channels_per_stream",C::Processing,V::Integer,1,max_route_channels,1,{}}};
constexpr ParameterDescriptor gate[]{{"data_type",C::Processing,V::FrameType,0,0,0,"mel"}};
constexpr ParameterSet sets[]{
    {NodeKind::SampleClock,clock},
    {NodeKind::Mixer,mixer},
    {NodeKind::InputSelector,selector},
    {NodeKind::Splitter,split},
    {NodeKind::Switch,route},
    {NodeKind::Gate,gate},
};
}
std::span<const ParameterDescriptor> routing_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
