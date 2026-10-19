#include "node_field_builder.h"
#include <limits>
namespace {
using namespace domain;
using namespace signal_contracts;
struct ConverterPolicy {
    template<class Node> auto& operator()(Node& node) const {return std::get<ConverterProcessingParameters>(node.parameters).policy;}
};
}
bool NodeParameterModel::resetSelectorSettings(domain::ConfiguredProcessingNode& node) const {
    node.inputs=_node.inputs;node.outputs=_node.outputs;
    const auto inputs=data_inputs(node),outputs=data_outputs(node);
    if(inputs.empty() || outputs.empty())return false;
    putParameter(node,"channels_per_stream",NumericLiteral{std::uint64_t(outputs.size())});
    putParameter(node,"selected_input",NumericLiteral{std::int64_t(inputs.front().id)});
    return true;
}
void NodeParameterModel::appendConverterPolicy() {
    const ConverterPolicy policy;
    _field_section=Section::Processing;
    choice("Rounding","numericRounding",{"Toward zero","Nearest ties to even"},[policy](const auto& node){return int(policy(node).rounding);},[policy](auto& node,int value){policy(node).rounding=Rounding(value);});
    choice("Overflow","numericOverflow",{"Reject","Saturate"},[policy](const auto& node){return int(policy(node).overflow);},[policy](auto& node,int value){policy(node).overflow=Overflow(value);});
    choice("Precision loss","numericPrecision",{"Reject inexact","Allow declared"},[policy](const auto& node){return int(policy(node).precision_loss);},[policy](auto& node,int value){policy(node).precision_loss=PrecisionLoss(value);});
    choice("Nonfinite values","numericNonfinite",{"Reject","Replace finite","Clamp finite"},[policy](const auto& node){return int(policy(node).nonfinite);},[policy](auto& node,int value){policy(node).nonfinite=NonFinite(value);});
}
void NodeParameterModel::appendConverterScaling() {
    const ConverterPolicy policy;
    _field_section=Section::Additional;
    choice("Scaling","numericScaling",{"Identity","Integer affine","Dyadic affine","PCM normalize","PCM denormalize","Binary64 affine"},[policy](const auto& node){return int(policy(node).scaling.mode);},[policy](auto& node,int value){policy(node).scaling.mode=ScaleMode(value);});
    literal("Gain","numericGain",[policy](const auto& node){return policy(node).scaling.gain.numerator;},[policy](auto& node,auto value){policy(node).scaling.gain.numerator=value;});
    literal("Offset","numericOffset",[policy](const auto& node){return policy(node).scaling.offset.numerator;},[policy](auto& node,auto value){policy(node).scaling.offset.numerator=value;});
    appendConverterExponents();
}
void NodeParameterModel::appendConverterExponents() {
    const ConverterPolicy policy;
    for(const bool gain:{true,false})integer(gain?"Gain exponent (base 2)":"Offset exponent (base 2)",gain?"numericGainExponent":"numericOffsetExponent",std::numeric_limits<std::int16_t>::min(),std::numeric_limits<std::int16_t>::max(),
        [gain,policy](const auto& node){const auto& scaling=policy(node).scaling;return gain?scaling.gain.exponent2:scaling.offset.exponent2;},
        [gain,policy](auto& node,int value){auto& scaling=policy(node).scaling;(gain?scaling.gain:scaling.offset).exponent2=std::int16_t(value);});
}
void NodeParameterModel::appendConverterReplacements() {
    const ConverterPolicy policy;
    literal("NaN replacement","numericNan",[policy](const auto& node){return policy(node).nan_replacement;},[policy](auto& node,auto value){policy(node).nan_replacement=value;});
    literal("Negative infinity replacement","numericNegativeInf",[policy](const auto& node){return policy(node).negative_inf_replacement;},[policy](auto& node,auto value){policy(node).negative_inf_replacement=value;});
    literal("Positive infinity replacement","numericPositiveInf",[policy](const auto& node){return policy(node).positive_inf_replacement;},[policy](auto& node,auto value){policy(node).positive_inf_replacement=value;});
}
bool NodeParameterModel::resetSettings() {
    auto node=numeric_editor::default_node(QString::fromStdString(_node.kind));node.id=_node.id;node.enabled=_node.enabled;
    if(const auto kind=domain::node_kind(_node.kind);kind && domain::is_scalar_view(*kind))node.inputs=_node.inputs;
    if(_node.kind=="InputSelector" && !resetSelectorSettings(node))return false;
    return commit(std::move(node));
}
void NodeParameterModel::append_conversion_fields() {
    _field_section=Section::Additional;
    if(std::holds_alternative<domain::ConverterProcessingParameters>(_node.parameters)){
        appendConverterPolicy();appendConverterScaling();appendConverterReplacements();
    }
    auto reset=makeField("Reset node settings","numericResetSettings",Editor::Action);
    reset.write=[this](const Value&){return resetSettings();};_fields.push_back(std::move(reset));
}
