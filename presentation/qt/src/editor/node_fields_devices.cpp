#include "node_field_builder.h"
#include <algorithm>
#include <limits>
QJsonValue NodeParameterModel::adapterValue(const QString& key,bool endpoint) const {
    return endpoint?_adapter.value("remote_endpoint").toObject().value(key):_adapter.value(key);
}
bool NodeParameterModel::writeAdapterValue(const QString& key,bool endpoint,const QJsonValue& value) {
    auto adapter=_adapter;
    if(endpoint){
        auto remote=adapter.value("remote_endpoint").toObject();
        if(value.isUndefined())remote.remove(key);else remote[key]=value;
        if(remote.isEmpty())adapter.remove("remote_endpoint");else adapter["remote_endpoint"]=remote;
    }else if(value.isUndefined())adapter.remove(key);else adapter[key]=value;
    return adapter==_adapter || (_edit_adapter && _edit_adapter(adapter));
}
void NodeParameterModel::appendAdapterText(const char* label,const char* key,const char* name,bool endpoint) {
    auto field=makeField(label,name,Editor::Text);
    field.read=[this,key,endpoint]()->Value{return adapterValue(key,endpoint).toString();};
    field.write=[this,key,endpoint](const Value& value){
        const auto text=std::get<QString>(value);
        return writeAdapterValue(key,endpoint,text.isEmpty()?QJsonValue(QJsonValue::Undefined):QJsonValue(text));
    };
    _fields.push_back(std::move(field));
}
void NodeParameterModel::appendAdapterNumber(const char* label,const char* key,const char* name,int minimum,int maximum,bool endpoint) {
    auto field=makeField(label,name,Editor::Integer);field.minimum=minimum;field.maximum=maximum;
    field.special_value_text="Unspecified";
    field.read=[this,key,minimum,endpoint]()->Value{return adapterValue(key,endpoint).toInt(minimum);};
    field.write=[this,key,minimum,endpoint](const Value& value){
        const auto number=std::get<int>(value);
        return writeAdapterValue(key,endpoint,number==minimum?QJsonValue(QJsonValue::Undefined):QJsonValue(number));
    };
    _fields.push_back(std::move(field));
}
void NodeParameterModel::appendRemoteEndpointFields() {
    appendAdapterText("Remote graph ID","graph_id","adapterGraphId",true);
    appendAdapterNumber("Remote node ID","node_id","adapterNodeId",-1,std::numeric_limits<int>::max(),true);
    appendAdapterNumber("Remote port index","port_index","adapterPortIndex",-1,65535,true);
    auto field=makeField("Remote port direction","adapterDirection",Editor::Choice);
    field.options={{"Unspecified",0},{"Input port",1},{"Output port",2}};
    field.read=[this]()->Value{const auto direction=adapterValue("direction",true).toString();return direction=="input"?1:direction=="output"?2:0;};
    field.write=[this](const Value& value){
        const auto direction=std::get<int>(value);
        return writeAdapterValue("direction",true,direction==1?QJsonValue("input"):direction==2?QJsonValue("output"):QJsonValue(QJsonValue::Undefined));
    };
    _fields.push_back(std::move(field));
}
void NodeParameterModel::appendNetworkAdapterFields() {
    appendAdapterText("Provider UUID","provider_uuid","adapterProviderUuid");
    appendAdapterText("Stream ID","stream_id","adapterStreamId");
    appendAdapterText("Remote host","host","adapterHost");
    appendAdapterNumber("Remote service port","port","adapterPort",0,65535);
    appendAdapterText("Device identity","device_identity","adapterDeviceIdentity");
    appendRemoteEndpointFields();
}
QString NodeParameterModel::captureSavedFormat() const {
    QString rate="?";
    if(!_node.outputs.empty()){
        const auto contract=_node.outputs[0].contract.view();
        if(const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract))rate=QString::number(numeric->clock.sample_rate);
    }
    return QString::number(domain::data_outputs(_node).size())+" × mono · "+rate+" Hz";
}
NodeParameterModel::Field NodeParameterModel::captureDeviceField(QString& error) {
    auto field=makeField("Capture device","captureDevice",Editor::Choice);
    field.options.push_back({"Follow system default",QString{},true});
    const auto choices=_capture_devices?_capture_devices(numeric_editor::capture_source_kind(_node),&error):QJsonArray{};
    for(const auto& value:choices){
        const auto device=value.toObject();
        if(!device.value("id").toString().isEmpty())field.options.push_back({device.value("name").toString(),device.value("id").toString(),false});
    }
    const auto selected=_adapter.value("device_identity").toString();
    const bool found=std::any_of(field.options.begin(),field.options.end(),[&](const auto& option){return std::get<QString>(option.value)==selected;});
    if(!found){field.options.push_back({"Unavailable device: %1",selected,true,{selected}});if(error.isEmpty())error="Selected capture device is unavailable";}
    return field;
}
void NodeParameterModel::appendCaptureChoice() {
    QString error;auto field=captureDeviceField(error);
    field.read=[this]()->Value{return _adapter.value("device_identity").toString();};
    field.write=[this](const Value& value){const auto text=std::get<QString>(value);return writeAdapterValue("device_identity",false,text.isEmpty()?QJsonValue(QJsonValue::Undefined):QJsonValue(text));};
    _fields.push_back(std::move(field));
    auto status=makeField("Capture device status","captureDeviceError",Editor::Caption);status.pinnable=false;status.translate_value=true;
    status.read=[error]()->Value{return error;};_fields.push_back(std::move(status));
}
void NodeParameterModel::appendCaptureFields() {
    auto format=makeField("Saved source format","captureSavedFormat",Editor::Caption);format.pinnable=false;
    format.read=[this]()->Value{return captureSavedFormat();};_fields.push_back(std::move(format));
    appendCaptureChoice();
    auto refresh=makeField("Refresh devices","refreshCaptureDevices",Editor::Action);
    refresh.write=[this](const Value&){buildFields();emit structureChanged();return true;};_fields.push_back(std::move(refresh));
}
void NodeParameterModel::append_devices_fields() {
    _field_section=Section::Route;
    if(_node.kind=="NetworkInput")appendNetworkAdapterFields();
    if(_node.kind=="AudioSource")appendCaptureFields();
}
