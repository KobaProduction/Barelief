#include "node_field_builder.h"
#include <domain/graph/processing_model.h>
#include <algorithm>
#include <stdexcept>
namespace {
using namespace domain;
std::size_t selector_streams(const ConfiguredProcessingNode& node) {
    const auto outputs=data_outputs(node).size();return outputs?data_inputs(node).size()/outputs:0;
}
int geometry_limit(std::optional<std::size_t> ports,std::size_t parallel_ports) {
    return int(ports.value_or(domain::max_variadic_inputs)/std::max(std::size_t{1},parallel_ports));
}
}
void NodeParameterModel::appendSelectorFields() {
    integer("Stream count","numericInputs",1,geometry_limit(profilePortLimit(),domain::data_outputs(_node).size()),
        [](const auto& node){return int(selector_streams(node));},
        [](auto& node,int value){if(!numeric_editor::set_input_count(node,std::uint16_t(value)))throw std::invalid_argument("count");});
    requireProfile();
    const auto bounds=domain::processing_parameter_numeric_bounds(_node,"channels_per_stream");if(!bounds)return;
    const int maximum=std::min(int(bounds->maximum),geometry_limit(profilePortLimit(),selector_streams(_node)));
    integer("Values per stream","numericSelectorChannels",int(bounds->minimum),maximum,
        [](const auto& node){return int(domain::data_outputs(node).size());},
        [](auto& node,int value){if(!numeric_editor::set_selector_channels(node,std::uint16_t(value)))throw std::invalid_argument("channels");});
    requireProfile();
}
void NodeParameterModel::appendRoutingOutputCount() {
    const auto bounds=domain::processing_parameter_numeric_bounds(_node,"outputs");if(!bounds)return;
    const auto& route=std::get<domain::RouteProcessingParameters>(_node.parameters);
    const int maximum=std::min(int(bounds->maximum),geometry_limit(profilePortLimit(),route.channels_per_stream));
    integer(_node.kind=="Switch"?"Stream count":"Output count","numericOutputs",int(bounds->minimum),maximum,
        [](const auto& node){return int(std::get<domain::RouteProcessingParameters>(node.parameters).outputs);},
        [](auto& node,int value){if(!numeric_editor::set_output_count(node,std::uint16_t(value)))throw std::invalid_argument("count");});
    requireProfile();
}
void NodeParameterModel::appendSwitchFields() {
    const auto bounds=domain::processing_parameter_numeric_bounds(_node,"channels_per_stream");if(!bounds)return;
    const auto& route=std::get<domain::RouteProcessingParameters>(_node.parameters);
    integer("Channels per stream","numericRouteChannels",int(bounds->minimum),std::min(int(bounds->maximum),geometry_limit(profilePortLimit(),route.outputs)),
        [](const auto& node){return std::get<domain::RouteProcessingParameters>(node.parameters).channels_per_stream;},
        [](auto& node,int value){if(!numeric_editor::set_route_channels(node,std::uint16_t(value)))throw std::invalid_argument("channels");});
    requireProfile();
    QStringList groups;for(unsigned i=0;i<route.outputs;++i)groups.push_back(QString::number(i+1));
    choice("Active output stream","numericSelected",groups,
        [](const auto& node){return std::get<domain::RouteProcessingParameters>(node.parameters).selected;},
        [](auto& node,int value){std::get<domain::RouteProcessingParameters>(node.parameters).selected=std::uint16_t(value);});
}
void NodeParameterModel::append_routing_fields() {
    if(_node.kind=="ArrayMultiply"){integer("Factor count","factorCount",2,int(std::min(domain::max_variadic_inputs,profilePortLimit().value_or(domain::max_variadic_inputs))),
        [](const auto& node){return int(domain::data_inputs(node).size());},
        [](auto& node,int count){if(!numeric_editor::set_input_count(node,std::uint16_t(count)))throw std::invalid_argument("factors");});requireProfile();}
    if(_node.kind=="InputSelector")appendSelectorFields();
    if(!std::holds_alternative<domain::RouteProcessingParameters>(_node.parameters))return;
    if(_node.kind=="Splitter" || _node.kind=="Switch")appendRoutingOutputCount();
    if(_node.kind=="Switch")appendSwitchFields();
    if(_node.kind=="Gate")choice("Gate","numericGate",{"Closed","Open"},
        [](const auto& node){return std::get<domain::RouteProcessingParameters>(node.parameters).open?1:0;},
        [](auto& node,int value){std::get<domain::RouteProcessingParameters>(node.parameters).open=value==1;});
}
