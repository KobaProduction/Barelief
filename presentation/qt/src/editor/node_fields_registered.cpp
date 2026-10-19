#include "node_field_builder.h"
#include "parameter_field_specs.h"
#include "numeric_graph_draft.h"
#include <project_storage/numeric_array_codec.h>
#include <algorithm>
#include <stdexcept>
using namespace domain;using namespace signal_contracts;
namespace {
bool matches_parameter_kind(const ProcessingParameterValue& value,ParameterValueKind kind) {
    switch(kind) {
    case ParameterValueKind::Boolean:return std::holds_alternative<bool>(value);
    case ParameterValueKind::Integer:case ParameterValueKind::Number:return std::holds_alternative<NumericLiteral>(value);
    case ParameterValueKind::String:case ParameterValueKind::WindowFunction:return std::holds_alternative<std::string>(value);
    case ParameterValueKind::NumericArray:return std::holds_alternative<NumericArray>(value);
    case ParameterValueKind::FrameType:return false;
    }
    return false;
}
}
ProcessingParameterValue NodeParameterModel::parameterValue(const ParameterDescriptor& descriptor) const {
    const auto* endpoint=std::get_if<EndpointProcessingParameters>(&_node.parameters);
    const auto& values=endpoint?endpoint->values:std::get<TransformProcessingParameters>(_node.parameters).values;
    const auto found=std::find_if(values.begin(),values.end(),[&](const auto& p){return p.key==descriptor.key;});
    if(found!=values.end())return found->value;
    if(descriptor.value_kind==ParameterValueKind::Integer)return NumericLiteral{std::int64_t(descriptor.default_number)};
    if(descriptor.value_kind==ParameterValueKind::Number)return NumericLiteral{descriptor.default_number};
    if(descriptor.value_kind==ParameterValueKind::Boolean)return descriptor.default_number!=0;
    if(descriptor.value_kind==ParameterValueKind::NumericArray)return NumericArray{std::vector<float>(descriptor.default_array.begin(),descriptor.default_array.end())};
    return std::string(descriptor.default_text);
}
void NodeParameterModel::putParameter(ConfiguredProcessingNode& n,std::string_view key,ProcessingParameterValue value) {
    if(n.kind=="Number" && key=="value"){
        const auto type=std::visit([](auto v){using T=decltype(v);if constexpr(std::is_same_v<T,std::int64_t>)return DType::Int64;else if constexpr(std::is_same_v<T,std::uint64_t>)return DType::UInt64;else if constexpr(std::is_same_v<T,float>)return DType::Float32;else return DType::Float64;},std::get<NumericLiteral>(value));
        if(!numeric_editor::set_dtype(n,PortDirection::Output,0,type))throw std::invalid_argument("number type");
    }
    auto* endpoint=std::get_if<EndpointProcessingParameters>(&n.parameters);
    auto& values=endpoint?endpoint->values:std::get<TransformProcessingParameters>(n.parameters).values;
    const auto found=std::find_if(values.begin(),values.end(),[&](const auto& p){return p.key==key;});
    if(found==values.end())values.push_back({std::string(key),std::move(value)});else found->value=std::move(value);
}
bool NodeParameterModel::parameterVisible(const ParameterDescriptor& descriptor) const {
    if(descriptor.classification!=ParameterClassification::Processing || descriptor.key=="enabled" || descriptor.value_kind==ParameterValueKind::FrameType)return false;
    if(_node.kind=="AudioSource" && descriptor.key=="sample_rate")return false;
    if(_node.kind=="InputSelector" && descriptor.key=="channels_per_stream")return false;
    if(_node.kind=="FFTView" && descriptor.key=="bin_hz")return false;
    if(_node.kind=="AmplitudePhase" && descriptor.key=="fft_size")return false;
    return true;
}
void NodeParameterModel::append_registered_fields() {
    const auto kind=node_kind(_node.kind);
    if(!kind || (!std::holds_alternative<EndpointProcessingParameters>(_node.parameters) && !std::holds_alternative<TransformProcessingParameters>(_node.parameters)))return;
    for(const auto& descriptor:parameter_descriptors(*kind))if(parameterVisible(descriptor))appendParameterField(descriptor);
}
void NodeParameterModel::appendParameterField(const ParameterDescriptor& descriptor) {
    const auto key=QString::fromUtf8(descriptor.key.data(),int(descriptor.key.size()));
    const auto title=numeric_editor::parameter_title(key);
    if(!matches_parameter_kind(parameterValue(descriptor),descriptor.value_kind)) {
        auto field=makeField(title,key,Editor::Caption);
        field.pinnable=false;
        field.translate_value=true;
        field.read=[]()->Value{return QStringLiteral("Invalid parameters");};
        _fields.push_back(std::move(field));
        return;
    }
    const auto spec=numeric_editor::parameter_choices(QString::fromStdString(_node.kind),key);
    if(!spec.values.empty())appendParameterChoice(descriptor,title,spec.values,spec.labels);
    else appendParameterValue(descriptor,title);
    auto& f=_fields.back();
    if(const auto bounds=processing_parameter_numeric_bounds(_node,descriptor.key)){f.numeric_minimum=bounds->minimum;f.numeric_maximum=bounds->maximum;}
    if(_node.kind=="MatrixCenter" && descriptor.key=="offset")f.available=[this]{
        const auto* method=parameter_descriptor(NodeKind::MatrixCenter,"method");
        if(!method)return false;
        const auto value=parameterValue(*method);
        const auto* numeric=std::get_if<NumericLiteral>(&value);
        return numeric && std::visit([](auto number){return number==2;},*numeric);
    };
}
void NodeParameterModel::appendParameterChoice(const ParameterDescriptor& descriptor,const QString& title,const QStringList& values,const QStringList& labels) {
    const auto key=QString::fromUtf8(descriptor.key.data(),int(descriptor.key.size()));
    auto f=makeField(title,key,Editor::Choice);
    for(int i=0;i<values.size();++i)f.options.push_back({labels.value(i,values[i]),i,true});
    f.read=[this,descriptor,values]()->Value{const auto value=parameterValue(descriptor);const auto* number=std::get_if<NumericLiteral>(&value);
        const auto text=number?QString::number(std::visit([](auto v){return double(v);},*number),'g',16):QString::fromStdString(std::get<std::string>(value));
        return std::max(0,int(values.indexOf(text)));};
    f.write=[this,descriptor,values](const Value& value){const auto index=std::get<int>(value);if(index<0 || index>=values.size())return false;auto n=_node;
        if(descriptor.value_kind==ParameterValueKind::Integer)putParameter(n,descriptor.key,NumericLiteral{values[index].toLongLong()});
        else putParameter(n,descriptor.key,values[index].toStdString());return commit(std::move(n));};
    _fields.push_back(std::move(f));
}
void NodeParameterModel::appendParameterValue(const ParameterDescriptor& descriptor,const QString& title) {
    const auto key=QString::fromUtf8(descriptor.key.data(),int(descriptor.key.size()));
    auto get=[this,descriptor](const auto&){return parameterValue(descriptor);};
    auto put=[key](auto& n,ProcessingParameterValue value){putParameter(n,key.toStdString(),std::move(value));};
    if(descriptor.value_kind==ParameterValueKind::Integer || descriptor.value_kind==ParameterValueKind::Number){
        literal(title,key,[get](const auto& n){return std::get<NumericLiteral>(get(n));},[put](auto& n,NumericLiteral v){put(n,v);},descriptor.value_kind==ParameterValueKind::Integer?std::optional<int>{domain::numeric_constant_kind(_node.kind)?1:0}:std::nullopt);
    }else if(descriptor.value_kind==ParameterValueKind::Boolean)
        boolean(title,key,[get](const auto& n){return std::get<bool>(get(n));},[put](auto& n,bool v){put(n,v);});
    else if(descriptor.value_kind==ParameterValueKind::NumericArray)
        text(title,key,[get](const auto& n){return numeric_editor::numeric_array_text(std::get<NumericArray>(get(n)));},[put](auto& n,const QString& text){NumericArray value;if(project_storage::parse_numeric_array(text.toStdString(),value)!=Status::ok)throw std::invalid_argument("array values");put(n,std::move(value));});
    else text(title,key,[get](const auto& n){return QString::fromStdString(std::get<std::string>(get(n)));},[put](auto& n,const QString& v){put(n,v.toStdString());});
}
