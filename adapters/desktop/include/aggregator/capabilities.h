#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QSet>
#include <bdsp_protocol/numeric_control_message.h>
#include <bdsp_protocol/numeric_data_message.h>
namespace aggregator_capabilities {
constexpr quint8 PacketId=16;
inline bool text(const QString& value,int maximum=256){return !value.isEmpty() && value.toUtf8().size()<=maximum && !value.contains(QChar(0));}
inline bool integer(const QJsonValue& value,int minimum,int maximum){return value.isDouble() && value.toDouble()==value.toInt(-1) && value.toInt(-1)>=minimum && value.toInt(-1)<=maximum;}
inline bool decimal(const QJsonValue& value,bool positive){bool ok=false;const auto s=value.toString();const auto n=s.toULongLong(&ok);return ok && (!positive || n) && QString::number(n)==s;}
// Directory metadata locates an endpoint. Only CONTROL negotiation and actual
// domain/kernel admission authorize a processing combination or DATA binding.
inline bool valid(const QJsonObject& object) {
    if(!integer(object["version"],3,3) || QUuid(object["uuid"].toString()).isNull() ||
       !integer(object["control_version"],bdsp_protocol::numeric_control::SchemaVersion,bdsp_protocol::numeric_control::SchemaVersion) || !integer(object["data_version"],bdsp_protocol::numeric_data::PayloadVersion,bdsp_protocol::numeric_data::PayloadVersion) ||
       !integer(object["stream_port"],0,65535) || !decimal(object["publisher_incarnation"],true) ||
       !decimal(object["graph_revision"],false) || !object["streams"].isArray())return false;
    const auto streams=object["streams"].toArray();if(streams.size()>128)return false;
    QSet<QString> identifiers,ports;
    for(const auto value:streams) {
        if(!value.isObject())return false;const auto stream=value.toObject();
        const auto id=stream["id"].toString(),graph=stream["graph_id"].toString(),direction=stream["direction"].toString();
        if(!text(id) || !text(graph) || identifiers.contains(id) ||
           !integer(stream["node_id"],0,INT_MAX) || !integer(stream["port_index"],0,65535) ||
           (direction!="input" && direction!="output") || !integer(stream["payload_version"],bdsp_protocol::numeric_data::PayloadVersion,bdsp_protocol::numeric_data::PayloadVersion))return false;
        const auto identity=QString::fromLatin1(QJsonDocument(QJsonArray{graph,stream["node_id"],direction,stream["port_index"]}).toJson(QJsonDocument::Compact).toHex());
        if(ports.contains(identity))return false;identifiers.insert(id);ports.insert(identity);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact).size()<=32768;
}
inline QJsonObject stream_bindings(const QJsonArray& streams) {
    QJsonObject result;for(const auto value:streams){const auto stream=value.toObject();QJsonObject identity;
        for(const auto key:{"graph_id","node_id","direction","port_index","payload_version"})identity[key]=stream[key];result[stream["id"].toString()]=identity;}
    return result;
}
inline bool same_stream_bindings(const QJsonArray& a,const QJsonArray& b){return stream_bindings(a)==stream_bindings(b);}
inline QByteArray encode(const QJsonObject& object){return QJsonDocument(object).toJson(QJsonDocument::Compact);}
inline bool decode(const QByteArray& bytes,QJsonObject& out){if(bytes.size()>32768)return false;const auto doc=QJsonDocument::fromJson(bytes);if(!doc.isObject() || !valid(doc.object()))return false;out=doc.object();return true;}
}
