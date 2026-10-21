#pragma once
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/scheme_editor/numeric_node_operations.h>
#include <barelief/ui_text.h>
template<class Read,class Write>
void NodeParameterModel::literal(QString label,QString key,Read read,Write write,std::optional<int> fixed_type) {
    auto f=makeField(label,key,Editor::Literal);
    f.literal_type=fixed_type;
    f.read=[this,read]()->Value{return read(_node);};
    f.write=[this,write](const Value& v){auto n=_node;write(n,std::get<signal_contracts::NumericLiteral>(v));return commit(std::move(n));};
    _fields.push_back(std::move(f));
}
namespace numeric_editor::detail {
inline std::vector<NodeParameterModel::Option> selector_options(const domain::ConfiguredProcessingNode& node) {
    std::vector<NodeParameterModel::Option> options;
    const auto outputs=domain::data_outputs(node).size();
    if(outputs>1)for(std::size_t stream=0;stream<node.inputs.size()/outputs;++stream)
        options.push_back({"Stream %1",int(stream),true,{QString::number(stream+1)}});
    else for(const auto& port:node.inputs)
        options.push_back({"Input %1",int(port.id),true,{QString::number(unsigned(port.id)+1)}});
    return options;
}
}
template<class Read,class Write>
void NodeParameterModel::choice(QString label,QString key,QStringList names,Read read,Write write) {
    auto f=makeField(label,key,Editor::Choice);
    if(_node.kind=="InputSelector" && key=="numericSelected")
        f.options=numeric_editor::detail::selector_options(_node);
    else for(int i=0;i<names.size();++i)f.options.push_back({names[i],i,true});
    f.read=[this,read]()->Value{return int(read(_node));};
    f.write=[this,write](const Value& v){auto n=_node;write(n,std::get<int>(v));return commit(std::move(n));};
    _fields.push_back(std::move(f));
}
template<class Read,class Write>
void NodeParameterModel::boolean(QString label,QString key,Read read,Write write) {
    auto f=makeField(label,key,Editor::Boolean);
    f.read=[this,read]()->Value{return bool(read(_node));};
    f.write=[this,write](const Value& v){auto n=_node;write(n,std::get<bool>(v));return commit(std::move(n));};
    _fields.push_back(std::move(f));
}
template<class Read,class Write>
void NodeParameterModel::text(QString label,QString key,Read read,Write write) {
    auto f=makeField(label,key,Editor::Text);
    f.read=[this,read]()->Value{return read(_node);};
    f.write=[this,write](const Value& v){auto n=_node;write(n,std::get<QString>(v));return commit(std::move(n));};
    _fields.push_back(std::move(f));
}
template<class Read,class Write>
void NodeParameterModel::integer(QString label,QString key,int minimum,int maximum,Read read,Write write) {
    auto f=makeField(label,key,Editor::Integer);
    f.minimum=minimum;f.maximum=maximum;
    f.read=[this,read]()->Value{return int(read(_node));};
    f.write=[this,write](const Value& v){auto n=_node;write(n,std::get<int>(v));return commit(std::move(n));};
    _fields.push_back(std::move(f));
}
