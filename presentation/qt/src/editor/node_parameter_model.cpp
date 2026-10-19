#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include "numeric_graph_draft.h"
#include "parameter_field_specs.h"
#include <domain/graph/node_factory.h>
#include "barelief/ui_text.h"
#include <core_app/spectrum_projection_ui.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cmath>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
NodeParameterModel::NodeParameterModel(QObject *parent) : QObject(parent) {}
void NodeParameterModel::setEditHandlers(std::function<bool(const domain::ConfiguredProcessingNode &)> node,
                                         std::function<bool(const QJsonObject &)> adapter,
                                         std::function<bool(const QString &, bool)> presentation) {
    _edit_node = std::move(node);
    _edit_adapter = std::move(adapter);
    _edit_presentation = std::move(presentation);
}
namespace {
bool same_parameter_ports(const std::vector<domain::ProcessingPort>& before,const std::vector<domain::ProcessingPort>& after) {
    if(before.size()!=after.size())return false;
    for(std::size_t i=0;i<before.size();++i){
        if(before[i].id!=after[i].id || !before[i].contract || !after[i].contract)return false;
        const auto previous=before[i].contract.view(),next=after[i].contract.view();
        if(previous.index()!=next.index())return false;
        const auto* numeric=std::get_if<signal_contracts::NumericContract>(&previous);
        if(numeric && numeric->shape.rank!=std::get<signal_contracts::NumericContract>(next).shape.rank)return false;
    }
    return true;
}
}
bool NodeParameterModel::requiresStructure(const domain::ConfiguredProcessingNode& node) const {
    if(!same_parameter_ports(_node.inputs,node.inputs) || !same_parameter_ports(_node.outputs,node.outputs))return true;
    if(node.kind=="ArrayFill"){
        std::size_t before=0,after=0;domain::array_generator_values(_node,{},before);domain::array_generator_values(node,{},after);
        if(bool(before)!=bool(after))return true;
    }
    const auto* oldTones=std::get_if<domain::SignalGeneratorProcessingParameters>(&_node.parameters);
    const auto* newTones=std::get_if<domain::SignalGeneratorProcessingParameters>(&node.parameters);
    return oldTones && newTones && oldTones->oscillators.size()!=newTones->oscillators.size();
}
void NodeParameterModel::copyOscillatorDrafts(std::size_t from,std::size_t to,QMap<QString,EditState>& remapped) const {
    const auto prefix=QString("oscillator%1").arg(from);
    for(auto it=_edits.begin();it!=_edits.end();++it){
        if(!it.key().startsWith(prefix))continue;
        const auto suffix=it.key().sliced(prefix.size());
        if(suffix.isEmpty() || suffix.front().isDigit())continue;
        remapped[QString("oscillator%1").arg(to)+suffix]=it.value();
    }
}
bool NodeParameterModel::remapOscillatorDrafts(const domain::SignalGeneratorProcessingParameters& before,const domain::SignalGeneratorProcessingParameters& after) {
    auto remapped=_edits;
    clearOscillatorDrafts(remapped);
    bool reordered=copyMappedOscillatorDrafts(before,after,remapped);
    if(before.oscillators.size()==after.oscillators.size())
        for(std::size_t i=0;i<before.oscillators.size();++i)reordered=reordered || before.oscillators[i].id!=after.oscillators[i].id;
    _edits=std::move(remapped);return reordered;
}
void NodeParameterModel::clearOscillatorDrafts(QMap<QString,EditState>& remapped) const {
    for(auto it=remapped.begin();it!=remapped.end();)
        if(it.key().startsWith("oscillator") && it.key()!="oscillatorCount")it=remapped.erase(it);else ++it;
}
bool NodeParameterModel::copyMappedOscillatorDrafts(const domain::SignalGeneratorProcessingParameters& before,
    const domain::SignalGeneratorProcessingParameters& after,QMap<QString,EditState>& remapped) const {
    bool reordered=false;
    for(std::size_t i=0;i<before.oscillators.size();++i){
        const auto found=std::find_if(after.oscillators.begin(),after.oscillators.end(),[&](const auto& tone){return tone.id==before.oscillators[i].id;});
        if(found==after.oscillators.end())continue;
        const auto next=std::size_t(found-after.oscillators.begin());reordered=reordered || i!=next;
        copyOscillatorDrafts(i,next,remapped);
    }
    return reordered;
}
void NodeParameterModel::setNode(const domain::ConfiguredProcessingNode& node,bool rebuild) {
    const bool first=_fields.empty();rebuild=rebuild || requiresStructure(node);
    const auto* oldTones=std::get_if<domain::SignalGeneratorProcessingParameters>(&_node.parameters);
    const auto* newTones=std::get_if<domain::SignalGeneratorProcessingParameters>(&node.parameters);
    if(_node.id!=node.id)_edits.clear();
    else if(oldTones && newTones)rebuild=remapOscillatorDrafts(*oldTones,*newTones) || rebuild;
    _node=node;buildFields();
    if(rebuild || first)emit structureChanged();else emit valuesChanged();
}
void NodeParameterModel::setAdapter(QJsonObject a) {
    _adapter = std::move(a);
    if (!_node.kind.empty())
        buildFields();
    emit valuesChanged();
}
void NodeParameterModel::setPresentation(QJsonObject p) {
    _presentation = std::move(p);
    emit presentationChanged();
}
void NodeParameterModel::setCaptureDeviceProvider(std::function<QJsonArray(int, QString *)> p) {
    _capture_devices = std::move(p);
    if (!_node.kind.empty()) {
        buildFields();
        emit structureChanged();
    }
}
const NodeParameterModel::Field *NodeParameterModel::field(const QString &key) const {
    for (const auto &f : _fields)
        if (f.key == key)
            return &f;
    return nullptr;
}
void NodeParameterModel::setDraftInput(const QString &key, const QString &input, int type) {
    auto &state = _edits[key];
    state.input = input;
    state.literal_type = type;
    state.dirty = true;
    emit editStateChanged(key);
}
NodeParameterModel::EditState NodeParameterModel::editState(const QString &key) const {
    return _edits.value(key);
}
bool NodeParameterModel::commit(domain::ConfiguredProcessingNode n) {
    if(!domain::derive_processing_node_contracts(n,_admission_limits?*_admission_limits:numeric_editor::limits()))return false;
    return _edit_node && _edit_node(n);
}
void NodeParameterModel::setAdmissionLimits(std::optional<domain::ProcessingAdmissionLimits> limits) {
    _admission_limits=std::move(limits);buildFields();emit structureChanged();
}
std::optional<std::size_t> NodeParameterModel::profilePortLimit() const {
    if(!_admission_limits)return {};
    return _admission_limits->max_ports_per_node;
}
void NodeParameterModel::requireProfile() {
    _fields.back().available=[this]{return _admission_limits.has_value();};
    _fields.back().bounds_error="Value exceeds the active execution profile";
    _fields.back().availability_error="Execution profile is unavailable";
}
bool NodeParameterModel::supportsBypass() const {
    if(_node.inputs.empty() || _node.outputs.empty())return false;
    // Bypass is an identity operation, not a general-purpose enable/mute switch.
    const bool identity=_node.inputs.size()==1 && _node.outputs.size()==1 && signal_contracts::same_physical_contract(_node.inputs[0].contract.view(),_node.outputs[0].contract.view());
    if(_node.kind=="Converter")return identity;
    const auto kind=domain::node_kind(_node.kind);if(!kind)return false;
    switch(*kind){
    case domain::NodeKind::PcmResampler:case domain::NodeKind::WindowFunction:case domain::NodeKind::ArrayMultiply:
    case domain::NodeKind::MatrixWindowFunction:case domain::NodeKind::MatrixCenter:return true;
    case domain::NodeKind::Window:case domain::NodeKind::SpectrumGain:
    case domain::NodeKind::BarkWeighting:case domain::NodeKind::SpectrumMerge:case domain::NodeKind::FrequencyNormalizer:
    case domain::NodeKind::ScalarReduce:
        return identity;
    default:return false;
    }
}
bool NodeParameterModel::setBypassed(bool bypassed) {
    if(!supportsBypass())return !bypassed;
    if(bypassed==!_node.enabled)return true;
    auto n=_node;n.enabled=!bypassed;
    const bool accepted=commit(std::move(n));
    // Refused requests must restore every bound control as well as accepted
    // edits/undo updating the shared model through the ordinary command path.
    emit valuesChanged();
    return accepted;
}
namespace {
bool field_value_in_range(const NodeParameterModel::Field& field,const NodeParameterModel::Value& value) {
    using E=NodeParameterModel::Editor;
    if(field.editor==E::Integer){const auto number=std::get<int>(value);return number>=field.minimum && number<=field.maximum;}
    if(field.editor==E::Choice)return std::any_of(field.options.begin(),field.options.end(),[&](const auto& option){return option.value==value;});
    return true;
}
bool write_field(const NodeParameterModel::Field& field,const NodeParameterModel::Value& value) {
    try{return field_value_in_range(field,value) && field.write(value);}
    catch(const std::invalid_argument&) {return false;}
    catch(const std::bad_variant_access&) {return false;}
}
std::optional<signal_contracts::NumericLiteral> literal_type(int type) {
    using L=signal_contracts::NumericLiteral;
    switch(type){
    case 0:return L{std::int64_t{}};
    case 1:return L{std::uint64_t{}};
    case 2:return L{float{}};
    case 3:return L{double{}};
    default:return {};
    }
}
bool literal_in_bounds(const NodeParameterModel::Field& field,const signal_contracts::NumericLiteral& value) {
    return std::visit([&](auto number){
        const long double exact=number;
        return (!field.numeric_minimum || exact>=*field.numeric_minimum) && (!field.numeric_maximum || exact<=*field.numeric_maximum);
    },value);
}
QStringList literal_error_arguments(const NodeParameterModel::Field& field,const signal_contracts::NumericLiteral& value,int type) {
    const QStringList types{"int64","uint64","float32","float64"};
    auto low=std::visit([](auto number){using T=decltype(number);return numeric_editor::literal_text(signal_contracts::NumericLiteral{std::numeric_limits<T>::lowest()});},value);
    auto high=std::visit([](auto number){using T=decltype(number);return numeric_editor::literal_text(signal_contracts::NumericLiteral{std::numeric_limits<T>::max()});},value);
    if(field.numeric_minimum)low=QString::number(*field.numeric_minimum,'g',17);
    if(field.numeric_maximum)high=QString::number(*field.numeric_maximum,'g',17);
    return {types[type],low,high};
}
}
bool NodeParameterModel::finishEdit(const QString& key,bool accepted) {
    if(accepted)_edits.remove(key);else _edits[key].error="Unsupported node settings";
    emit editStateChanged(key);emit valuesChanged();return accepted;
}
bool NodeParameterModel::edit(const QString& key,const Value& value) {
    const auto* current=field(key);if(!current || !current->write || (current->available && !current->available()))return false;
    // A write can synchronously rebuild the descriptor list.
    const auto descriptor=*current;
    if(descriptor.editor==Editor::Text && std::holds_alternative<QString>(value)){
        _edits[key].input=std::get<QString>(value);_edits[key].dirty=true;
    }
    if(descriptor.read && descriptor.read()==value)return finishEdit(key,true);
    return finishEdit(key,write_field(descriptor,value));
}
bool NodeParameterModel::editLiteral(const QString& key,const QString& text,int type) {
    const auto* current=field(key);
    if(!current || current->editor!=Editor::Literal || (current->literal_type && type!=*current->literal_type))return false;
    _edits[key]=EditState{text,{},type,{},true};
    auto value=literal_type(type);if(!value)return false;
    if(numeric_editor::parse_literal(text,*value,*value) && literal_in_bounds(*current,*value))return edit(key,Value{*value});
    _edits[key].error="Enter a valid %1 value between %2 and %3.";
    _edits[key].error_arguments=literal_error_arguments(*current,*value,type);
    emit editStateChanged(key);return false;
}
bool NodeParameterModel::onBlock(const QString &key) const {
    const auto *f = field(key);
    return f && (f->primary || _presentation.value("inline_parameters").toArray().contains(key));
}
bool NodeParameterModel::setOnBlock(const QString &key, bool shown) {
    const auto* f=field(key);
    return f && f->pinnable && _edit_presentation && _edit_presentation(key, shown);
}
bool NodeParameterModel::expanded(Section s) const {
    const auto i = _expanded.find({_node.id, int(s)});
    return i != _expanded.end() ? i->second : int(s) < 2;
}
void NodeParameterModel::setExpanded(Section s, bool v) {
    _expanded[{_node.id, int(s)}] = v;
}
const char *NodeParameterModel::sectionTitle(Section s) {
    switch (s) {
    case Section::Route:
        return "Source / route";
    case Section::Processing:
        return "Processing";
    case Section::Contract:
        return "Data contract";
    default:
        return "Additional settings";
    }
}
NodeParameterModel::Field NodeParameterModel::makeField(QString label, QString key, Editor editor) {
        const auto _kind=QString::fromStdString(_node.kind);
        Field f;
        f.label = std::move(label);
        f.key = std::move(key);
        f.editor = editor;
        f.section = _field_section;
        f.order = int(_fields.size());
        f.primary=numeric_editor::parameter_is_primary(_kind,f.key);
        if(_kind=="ArrayFill" && f.key=="value"){std::size_t count=0;if(domain::array_generator_values(_node,{},count)==signal_contracts::Status::ok && count)f.primary=false;}
        f.hide_inline = _kind == "InputSelector" && (f.key == "numericSelected" || f.key == "numericInputs");
        return f;
    }
void NodeParameterModel::buildFields() {
    _fields.clear();_field_section=Section::Route;
    append_routing_fields();
    append_generators_fields();
    append_mixing_fields();
    append_music_fields();
    append_accumulation_fields();
    append_registered_fields();
    append_contracts_fields();
    append_conversion_fields();
    append_devices_fields();
    std::stable_sort(_fields.begin(),_fields.end(),[](const Field& a,const Field& b){return int(a.section)<int(b.section);});
}
