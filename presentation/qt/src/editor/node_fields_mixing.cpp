#include "node_field_builder.h"
#include <domain/graph/processing_model.h>
#include <dsp_kernels/weighted_mix.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cmath>
namespace {
using namespace domain;using namespace signal_contracts;
PortId next_mixer_port(const ConfiguredProcessingNode& node) {
    const auto maximum=std::max_element(node.inputs.begin(),node.inputs.end(),[](const auto& left,const auto& right){return left.id<right.id;});
    if(maximum==node.inputs.end() || maximum->id==std::numeric_limits<PortId>::max())throw std::invalid_argument("mixer port ID limit");
    return PortId(maximum->id+1);
}
void trim_mixer_weights(ConfiguredProcessingNode& node) {
    auto& weights=std::get<MixerProcessingParameters>(node.parameters).weights;
    std::erase_if(weights,[&](const auto& weight){return std::none_of(node.inputs.begin(),node.inputs.end(),[&](const auto& input){return input.id==weight.port;});});
}
void resize_mixer_inputs(ConfiguredProcessingNode& node,int count) {
    if(node.inputs.empty())throw std::invalid_argument("mixer input unavailable");
    auto& weights=std::get<MixerProcessingParameters>(node.parameters).weights;
    while(node.inputs.size()<std::size_t(count)){
        auto port=node.inputs.front();port.id=next_mixer_port(node);node.inputs.push_back(port);weights.push_back({port.id,1.});
    }
    node.inputs.resize(std::size_t(count));trim_mixer_weights(node);
}
NumericLiteral mixer_weight(const ConfiguredProcessingNode& node,PortId id) {
    for(const auto& weight:std::get<MixerProcessingParameters>(node.parameters).weights)if(weight.port==id)return NumericLiteral{weight.weight};
    return NumericLiteral{0.};
}
void write_mixer_weight(ConfiguredProcessingNode& node,PortId id,const NumericLiteral& literal) {
    auto& parameters=std::get<MixerProcessingParameters>(node.parameters);
    const double value=std::visit([](auto number){return double(number);},literal);
    if(!std::isfinite(value) || (domain::mixer_requires_nonnegative_weights(parameters.mode) && value<0))throw std::invalid_argument("weight");
    for(auto& weight:parameters.weights)if(weight.port==id){weight.weight=value;return;}
    throw std::invalid_argument("mixer weight unavailable");
}
}
void NodeParameterModel::appendMixerTiming() {
    choice("Input timing policy","mixerConsumption",{"Clocked FIFO","Source aligned"},
        [](const auto& node){return int(std::get<domain::MixerProcessingParameters>(node.parameters).consumption);},
        [](auto& node,int value){std::get<domain::MixerProcessingParameters>(node.parameters).consumption=domain::MixerConsumption(value);});
    const auto bounds=domain::processing_parameter_numeric_bounds(_node,"fifo_milliseconds");if(!bounds)return;
    integer("Input FIFO, ms","mixerFifo",int(bounds->minimum),int(bounds->maximum),
        [](const auto& node){return std::get<domain::MixerProcessingParameters>(node.parameters).fifo_milliseconds;},
        [](auto& node,int value){std::get<domain::MixerProcessingParameters>(node.parameters).fifo_milliseconds=std::uint32_t(value);});
}
QString NodeParameterModel::mixerShare(domain::PortId id) const {
    const auto& parameters=std::get<domain::MixerProcessingParameters>(_node.parameters);if(parameters.mode!=domain::MixerMode::WeightedMean)return {};
    std::vector<double> weights;weights.reserve(parameters.weights.size());
    std::size_t index=parameters.weights.size();
    for(const auto& weight:parameters.weights){if(weight.port==id)index=weights.size();weights.push_back(weight.weight);}
    const auto fraction=dsp_kernels::normalized_weight_fraction(weights,index);
    return fraction?QString::number(*fraction*100,'f',2)+" %":QString{};
}
void NodeParameterModel::appendMixerWeight(domain::PortId id) {
    literal("Input %1 weight",QString("mixerWeight%1").arg(id),
        [id](const auto& node){return mixer_weight(node,id);},[id](auto& node,const NumericLiteral& value){write_mixer_weight(node,id,value);});
    _fields.back().caption_arguments={QString::number(unsigned(id)+1)};
    auto share=makeField("Input %1 share",QString("mixerShare%1").arg(id),Editor::Caption);share.pinnable=false;
    share.caption_arguments={QString::number(unsigned(id)+1)};
    share.read=[this,id]()->Value{return mixerShare(id);};_fields.push_back(std::move(share));
}
void NodeParameterModel::append_mixing_fields() {
    if(!std::holds_alternative<domain::MixerProcessingParameters>(_node.parameters))return;
    choice("Mixing operation","mixerMode",{"Sum with gain","Normalized proportions","Weighted RMS","Maximum absolute"},
        [](const auto& node){return int(std::get<domain::MixerProcessingParameters>(node.parameters).mode);},
        [](auto& node,int value){std::get<domain::MixerProcessingParameters>(node.parameters).mode=domain::MixerMode(value);});
    const auto maximum=std::min(domain::max_variadic_inputs,profilePortLimit().value_or(domain::max_variadic_inputs));
    integer("Input count","mixerInputCount",1,int(maximum),[](const auto& node){return domain::data_inputs(node).size();},[](auto& node,int count){resize_mixer_inputs(node,count);});requireProfile();
    appendMixerTiming();
    for(const auto& weight:std::get<domain::MixerProcessingParameters>(_node.parameters).weights)appendMixerWeight(weight.port);
}
