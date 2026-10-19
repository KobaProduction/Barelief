#include "node_field_builder.h"
#include <signal_contracts/oscillator.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace {
using namespace domain;using namespace signal_contracts;
template<class Node> auto& oscillators(Node& node) {return std::get<SignalGeneratorProcessingParameters>(node.parameters).oscillators;}
struct OscillatorProperty {const char* key;const char* label;double OscillatorParameters::* value;};
constexpr std::array<OscillatorProperty,5> properties{{
    {"frequency_hz","Frequency, Hz",&OscillatorParameters::frequency_hz},{"amplitude","Amplitude",&OscillatorParameters::amplitude},
    {"phase_radians","Phase, radians",&OscillatorParameters::phase_radians},{"attack_seconds","Attack, seconds",&OscillatorParameters::attack_seconds},{"release_seconds","Release, seconds",&OscillatorParameters::release_seconds}}};
std::uint64_t next_oscillator_id(const std::vector<OscillatorParameters>& tones) {
    std::uint64_t next=1;
    for(const auto& tone:tones){if(tone.id==UINT64_MAX)throw std::invalid_argument("oscillator ID limit");next=std::max(next,tone.id+1);}
    return next;
}
void resize_oscillators(ConfiguredProcessingNode& node,int count) {
    auto& tones=oscillators(node);auto next=next_oscillator_id(tones);
    if(std::size_t(count)>tones.size() && std::uint64_t(std::size_t(count)-tones.size()-1)>UINT64_MAX-next)throw std::invalid_argument("oscillator ID limit");
    while(tones.size()<std::size_t(count)){
        OscillatorParameters tone;tone.id=next;tones.push_back(tone);if(tones.size()<std::size_t(count))++next;
    }
    tones.resize(std::size_t(count));
}
OscillatorParameters bank_tone(int bank,int index) {
    OscillatorParameters tone;tone.id=index+1;
    tone.frequency_hz=bank==0?440.:(bank==1?73.25+index*index*233.7:110.3+index*index*197.2);
    tone.amplitude=bank==0?.5:.045;tone.phase_radians=index*.27;return tone;
}
}
void NodeParameterModel::appendSampleClockFields() {
    const auto bounds=domain::processing_parameter_numeric_bounds(_node,"sample_rate");
    if(bounds)integer("Sample rate, Hz","clockSampleRate",int(bounds->minimum),int(bounds->maximum),
        [](const auto& node){return std::get<domain::SampleClockProcessingParameters>(node.parameters).sample_rate;},
        [](auto& node,int value){std::get<domain::SampleClockProcessingParameters>(node.parameters).sample_rate=std::uint32_t(value);});
    text("Clock domain","clockDomain",[](const auto& node){return QString::fromStdString(std::get<domain::SampleClockProcessingParameters>(node.parameters).domain);},
        [](auto& node,const QString& value){std::get<domain::SampleClockProcessingParameters>(node.parameters).domain=value.toStdString();});
}
bool NodeParameterModel::applyOscillatorBank(int bank) {
    auto node=_node;auto& tones=oscillators(node);tones.clear();const int count=bank==0?1:8;
    for(int i=0;i<count;++i)tones.push_back(bank_tone(bank,i));
    return commit(std::move(node));
}
void NodeParameterModel::appendOscillatorBankActions() {
    for(int bank=0;bank<3;++bank){
        auto preset=makeField(bank==0?"Single sine":bank==1?"Multitone bank 1":"Multitone bank 2",QString("generatorBank%1").arg(bank),Editor::Action);
        preset.pinnable=false;preset.write=[this,bank](const Value&){return applyOscillatorBank(bank);};_fields.push_back(std::move(preset));
    }
}
bool NodeParameterModel::addOscillator() {
    auto node=_node;auto& tones=oscillators(node);if(tones.size()>=signal_contracts::max_oscillators)return false;
    OscillatorParameters added;
    for(const auto& tone:tones){if(tone.id==UINT64_MAX)return false;added.id=std::max(added.id,tone.id+1);}
    tones.push_back(added);return commit(std::move(node));
}
void NodeParameterModel::appendOscillatorCount() {
    integer("Oscillator count","oscillatorCount",1,int(signal_contracts::max_oscillators),[](const auto& node){return oscillators(node).size();},
        [](auto& node,int count){resize_oscillators(node,count);});
    auto add=makeField("Add oscillator","addOscillator",Editor::Action);
    add.available=[this]{return oscillators(_node).size()<signal_contracts::max_oscillators;};
    add.write=[this](const Value&){return addOscillator();};_fields.push_back(std::move(add));
}
bool NodeParameterModel::removeOscillator(std::size_t index) {
    auto node=_node;auto& tones=oscillators(node);if(tones.size()<2 || index>=tones.size())return false;
    tones.erase(tones.begin()+std::ptrdiff_t(index));return commit(std::move(node));
}
void NodeParameterModel::appendOscillatorActions(std::size_t index) {
    const auto prefix=QString("oscillator%1").arg(index);
    auto caption=makeField("Oscillator ID",prefix+"Id",Editor::Caption);caption.pinnable=false;
    caption.read=[this,index]()->Value{return QString::number(oscillators(_node).at(index).id);};_fields.push_back(std::move(caption));
    auto remove=makeField("Remove oscillator",prefix+"Remove",Editor::Action);
    remove.available=[this]{return oscillators(_node).size()>1;};remove.write=[this,index](const Value&){return removeOscillator(index);};_fields.push_back(std::move(remove));
}
void NodeParameterModel::appendOscillatorProperty(std::size_t index,std::size_t property_index) {
    const auto property=properties[property_index];
    const auto key=QString("oscillator%1").arg(index)+QString::fromUtf8(property.key);
    literal(property.label,key,[index,property](const auto& node){return NumericLiteral{oscillators(node).at(index).*property.value};},
        [index,property](auto& node,const NumericLiteral& value){
            const double number=std::visit([](auto item){return double(item);},value);
            if(!std::isfinite(number))throw std::invalid_argument("finite oscillator parameter");
            oscillators(node).at(index).*property.value=number;
        });
}
void NodeParameterModel::appendOscillatorFields(std::size_t index) {
    const auto first=_fields.size();
    appendOscillatorActions(index);
    boolean("Oscillator enabled",QString("oscillator%1Enabled").arg(index),
        [index](const auto& node){return oscillators(node).at(index).enabled;},
        [index](auto& node,bool value){oscillators(node).at(index).enabled=value;});
    for(std::size_t i=0;i<properties.size();++i)appendOscillatorProperty(index,i);
    for(auto i=first;i<_fields.size();++i)_fields[i].hide_inspector=true;
}
void NodeParameterModel::append_generators_fields() {
    _field_section=Section::Processing;
    if(_node.kind=="SignalGenerator" || _node.kind=="Mixer" || _node.kind=="PcmResampler")
        integer(_node.kind=="PcmResampler"?"Synchronization source (-1: inherited)":"Synchronization source (-1: internal)","sampleClockNode",-1,std::numeric_limits<int>::max(),
            [](const auto& node){return node.clock_node;},[](auto& node,int id){node.clock_node=id;});
    if(std::holds_alternative<domain::SampleClockProcessingParameters>(_node.parameters))appendSampleClockFields();
    if(!std::holds_alternative<domain::SignalGeneratorProcessingParameters>(_node.parameters))return;
    appendOscillatorBankActions();appendOscillatorCount();
    const auto count=oscillators(_node).size();for(std::size_t i=0;i<count;++i)appendOscillatorFields(i);
}
