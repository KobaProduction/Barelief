#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QMap>
#include <cmath>
#include <limits>
#include <utility>

namespace configuration {
struct MigrationResult {
    QJsonObject document;
    QString error, error_path;
    int source_version=0, target_version=1;
    explicit operator bool() const { return error.isEmpty(); }
};
// Absence is legacy. Present null/string/fraction/zero is never a default.
inline bool version_number(const QJsonValue &value,int &number) {
    if(!value.isDouble())return false;
    const auto n=value.toDouble();
    if(!std::isfinite(n) || n<1 || n>std::numeric_limits<int>::max() || std::floor(n)!=n)return false;
    number=static_cast<int>(n);return true;
}
inline QString check_version(const QJsonObject &object,const QString &key,int maximum) {
    if(!object.contains(key))return {};
    int version=0;
    if(!version_number(object.value(key),version))return "Invalid "+key;
    return version>maximum ? "Unsupported "+key : QString{};
}
inline MigrationResult classify_document(const QJsonObject &raw) {
    MigrationResult result;result.document=raw;
    result.error=check_version(raw,"schema_version",1);result.error_path="schema_version";
    if(result && raw.contains("schema_version"))version_number(raw["schema_version"],result.source_version);
    return result;
}
inline QString active_path(const QString &explicit_path={}) {
    return QFileInfo(explicit_path.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("config.json")
                                             : explicit_path).absoluteFilePath();
}

// JSON format mapping only; no domain validation, runtime or application effects.
inline MigrationResult migration_error(MigrationResult result,const QString& message,const QString& path={}) {
    result.error=message;result.error_path=path;return result;
}
inline bool map_legacy_nodes(QJsonArray& nodes,QMap<int,QString>& kinds,int& next,MigrationResult& result) {
    for(int i=0;i<nodes.size();++i) {
        auto node=nodes[i].toObject();const auto id=node.value("id");
        const double value=id.toDouble(-1);
        if(!id.isDouble() || value<0 || value>=std::numeric_limits<int>::max()-3 ||
           std::floor(value)!=value || kinds.contains(int(value))) {
            result=migration_error(result,"Invalid legacy node ID or ID space exhausted","nodes/id");return false;
        }
        next=std::max(next,int(value)+1);auto presentation=node["internal-data"].toObject();
        if(presentation["model-name"]=="SurfaceView")presentation["model-name"]="SpectrogramView";
        kinds[int(value)]=presentation["model-name"].toString();node["internal-data"]=presentation;nodes[i]=node;
    }
    return true;
}
inline int append_legacy_node(QJsonArray& nodes,int& next,const QString& kind,const QJsonObject& near) {
    const int id=next++;nodes.append(QJsonObject{{"id",id},{"position",near.value("position").toObject()},
        {"internal-data",QJsonObject{{"model-name",kind}}}});return id;
}
inline QJsonObject legacy_edge(int from,int to) {
    return {{"outNodeId",from},{"inNodeId",to},{"outPortIndex",0},{"inPortIndex",0}};
}
inline bool valid_legacy_edge_ids(const QJsonObject& edge,const QMap<int,QString>& kinds) {
    const auto from=edge.value("outNodeId"),to=edge.value("inNodeId");
    const double source=from.toDouble(-1),target=to.toDouble(-1);
    return from.isDouble() && to.isDouble() && source>=0 && target>=0 &&
        source<=std::numeric_limits<int>::max() && target<=std::numeric_limits<int>::max() &&
        std::floor(source)==source && std::floor(target)==target &&
        kinds.contains(int(source)) && kinds.contains(int(target));
}
inline QJsonObject legacy_node_near(const QJsonArray& nodes,const QJsonValue& id) {
    for(const auto& node:nodes)if(node.toObject().value("id")==id)return node.toObject();
    return {};
}
inline bool legacy_capture_edge(const QJsonObject& edge,const QMap<int,QString>& kinds) {
    const int source=int(edge["outNodeId"].toDouble()),target=int(edge["inNodeId"].toDouble());
    return kinds.value(target)=="StereoFFT" &&
        (kinds.value(source)=="AudioSource" || kinds.value(source)=="TestSignal");
}
inline void insert_legacy_window_chain(QJsonArray& nodes,QJsonArray& edges,QJsonObject edge,int index,int& next) {
    const int target=int(edge["inNodeId"].toDouble());
    const auto near=legacy_node_near(nodes,edge["inNodeId"]);
    const int window=append_legacy_node(nodes,next,"Window",near);
    const int function=append_legacy_node(nodes,next,"WindowFunction",near);
    edge["inNodeId"]=window;edges[index]=edge;
    edges.append(legacy_edge(window,function));edges.append(legacy_edge(function,target));
}
inline bool rewrite_legacy_capture_edges(QJsonArray& nodes,QJsonArray& edges,const QMap<int,QString>& kinds,
                                         int& next,MigrationResult& result) {
    const auto original=edges;
    for(int i=0;i<original.size();++i) {
        auto edge=original[i].toObject();
        if(!valid_legacy_edge_ids(edge,kinds)) {
            result=migration_error(result,"Invalid legacy edge identity","connections");return false;
        }
        if(!legacy_capture_edge(edge,kinds))continue;
        if(edge["outPortIndex"]!=0 || edge["inPortIndex"]!=0){
            result=migration_error(result,"Unsupported legacy edge port","connections");return false;
        }
        insert_legacy_window_chain(nodes,edges,edge,i,next);
    }
    return true;
}
inline void append_legacy_bark_nodes(QJsonArray& nodes,QJsonArray& edges,const QMap<int,QString>& kinds,int& next) {
    for(auto it=kinds.cbegin();it!=kinds.cend();++it)if(it.value()=="StereoFFT") {
        const int bark=append_legacy_node(nodes,next,"BarkWeighting",{});
        for(int i=0;i<edges.size();++i){auto edge=edges[i].toObject();if(edge["outNodeId"]==it.key()){edge["outNodeId"]=bark;edges[i]=edge;}}
        edges.append(legacy_edge(it.key(),bark));
    }
}
inline bool prepare_legacy_graph(QJsonArray& nodes,QJsonArray& edges,MigrationResult& result) {
    QMap<int,QString> kinds;int next=0;
    if(!map_legacy_nodes(nodes,kinds,next,result))return false;
    if(qint64(next)+qint64(edges.size())*2+kinds.size()>=std::numeric_limits<int>::max()) {
        result=migration_error(result,"Legacy node ID space exhausted","nodes/id");return false;
    }
    if(!rewrite_legacy_capture_edges(nodes,edges,kinds,next,result))return false;
    append_legacy_bark_nodes(nodes,edges,kinds,next);
    return true;
}
inline QJsonObject mapped_legacy_graph(const QJsonObject& raw,const QJsonArray& nodes,const QJsonArray& edges) {
    auto mapped=raw;
    mapped["version"]=2;mapped["nodes"]=nodes;mapped["connections"]=edges;
    if(!mapped.contains("processing_version"))mapped["processing_version"]=1;
    return mapped;
}
inline MigrationResult migrate_legacy_graph(const QJsonObject& raw,MigrationResult result,const QString& marker) {
    if(raw.contains(marker))return migration_error(result,"Migration extension collision",marker);
    if(!raw.value("nodes").isArray() || !raw.value("connections").isArray())
        return migration_error(result,"Legacy graph arrays missing","nodes/connections");
    auto nodes=raw["nodes"].toArray();auto edges=raw["connections"].toArray();
    if(!prepare_legacy_graph(nodes,edges,result))return result;
    const auto mapped=mapped_legacy_graph(raw,nodes,edges);
    result.document=mapped;result.document[marker]=QJsonObject{{"source",raw},{"mapped",mapped}};
    return result;
}
inline MigrationResult restore_migration_source(const QJsonObject& raw,MigrationResult result,
                                               const QString& marker,const QString& error) {
    const auto record=raw.value(marker).toObject();auto current=raw;current.remove(marker);
    if(record.value("source").isObject() && record.value("mapped")==current) {
        result.document=record.value("source").toObject();return result;
    }
    return migration_error(result,error,marker);
}
inline MigrationResult classify_graph(const QJsonObject& raw,int target) {
    MigrationResult r;r.document=raw;r.target_version=target;
    r.error=check_version(raw,"version",2);r.error_path="version";
    if(r) {r.error=check_version(raw,"processing_version",1);r.error_path="processing_version";}
    return r;
}
inline MigrationResult migrate_graph(const QJsonObject &raw,int target=2) {
    auto r=classify_graph(raw,target);
    if(!r)return r;
    r.source_version=2; // Historical unversioned graph is the existing v2 shape.
    if(raw.contains("version"))version_number(raw["version"],r.source_version);
    if(target!=1 && target!=2)return migration_error(r,"Unsupported target graph version");
    if(r.source_version==target)return r;
    const QString marker="_configuration_graph_migration";
    if(target==1)return restore_migration_source(raw,r,marker,"Unsupported or lossy graph downgrade");
    return migrate_legacy_graph(raw,std::move(r),marker);
}

inline bool migrate_graph_value(QJsonValue& value,const QString& path,int target,MigrationResult& result) {
    if(!value.isObject())return true; // Malformed draft/value remains opaque until explicitly replaced.
    auto graph=value.toObject();auto mapped=migrate_graph(graph,target);
    if(!mapped){result=migration_error(result,mapped.error,path+"/"+mapped.error_path);return false;}
    graph=mapped.document;
    if(target==2 && !graph.contains("processing_version"))graph["processing_version"]=1;
    value=graph;return true;
}
inline bool migrate_preset_array(QJsonArray& presets,const QString& path,int target,MigrationResult& result) {
    for(int i=0;i<presets.size();++i){
        auto preset=presets[i].toObject();if(!preset.contains("graph"))continue;
        QJsonValue graph=preset.value("graph");
        if(!migrate_graph_value(graph,path+"/"+QString::number(i)+"/graph",target,result))return false;
        preset["graph"]=graph;presets[i]=preset;
    }
    return true;
}
inline bool migrate_graph_members(QJsonObject& object,const QString& path,int target,MigrationResult& result) {
    const QStringList keys{"pipeline_graph","scene_graph"};
    for(const auto& key:keys)if(object.contains(key)) {
        QJsonValue graph=object.value(key);
        if(!migrate_graph_value(graph,path+key,target,result))return false;
        object[key]=graph;
    }
    return true;
}
inline bool migrate_named_presets(QJsonObject& object,const QString& field,const QString& path,
                                  int target,MigrationResult& result) {
    if(!object.value(field).isArray())return true;
    auto presets=object.value(field).toArray();
    if(!migrate_preset_array(presets,path,target,result))return false;
    object[field]=presets;return true;
}
inline bool migrate_device_documents(QJsonObject& candidate,int target,MigrationResult& result) {
    if(!candidate.value("devices").isObject())return true;
    auto devices=candidate.value("devices").toObject();
    for(auto it=devices.begin();it!=devices.end();++it) {
        if(!it.value().isObject())continue;
        auto device=it.value().toObject();const auto path="devices/"+it.key()+"/";
        if(!migrate_graph_members(device,path,target,result) ||
           !migrate_named_presets(device,"presets",path+"presets",target,result))return false;
        it.value()=device;
    }
    candidate["devices"]=devices;return true;
}
inline void normalize_project_graphs(QJsonObject& candidate,const QJsonObject& raw) {
    if(raw.value("nodes").isArray()) {
        candidate.remove("nodes");candidate.remove("connections");candidate.remove("version");candidate.remove("processing_version");
        candidate["pipeline_graph"]=raw;candidate["scene_graph"]=raw;
    } else if(!raw.contains("pipeline_graph") && raw.value("scene_graph").isObject())
        candidate["pipeline_graph"]=raw.value("scene_graph");
}
inline bool migrate_project_graphs(QJsonObject& candidate,const QJsonObject& raw,int target,MigrationResult& result) {
    normalize_project_graphs(candidate,raw);
    if(!migrate_graph_members(candidate,{},target,result) ||
       !migrate_named_presets(candidate,"presets","presets",target,result))return false;
    auto ui=candidate.value("ui").toObject();
    if(!migrate_named_presets(ui,"pipeline_presets","ui/pipeline_presets",target,result))return false;
    if(candidate.value("ui").toObject().value("pipeline_presets").isArray())candidate["ui"]=ui;
    return migrate_device_documents(candidate,target,result);
}
inline MigrationResult install_project_migration(const QJsonObject& raw,MigrationResult result,
                                                const QJsonObject& candidate,const QString& marker) {
    if(result.source_version==0) {
        if(raw.contains(marker))return migration_error(result,"Migration extension collision",marker);
        result.document=candidate;result.document[marker]=QJsonObject{{"source",raw},{"mapped",candidate}};
    } else result.document=candidate;
    return result;
}
inline MigrationResult migrate_project_candidate(const QJsonObject& raw,MigrationResult result,
                                                int target_schema,int target_graph,const QString& marker) {
    auto candidate=raw;
    if(!migrate_project_graphs(candidate,raw,target_graph,result))return result;
    if(target_schema==0){result.document=candidate;return result;}
    candidate["schema_version"]=1;
    return install_project_migration(raw,result,candidate,marker);
}
inline MigrationResult migrate(const QJsonObject &raw,int target_schema=1,int target_graph=2) {
    auto r=classify_document(raw);r.target_version=target_schema;if(!r)return r;
    if(target_schema!=0 && target_schema!=1){r.error="Unsupported target schema";return r;}
    const QString marker="_configuration_project_migration";
    if(target_schema==0 && r.source_version==1)
        return restore_migration_source(raw,r,marker,"Unsupported or lossy project downgrade");
    return migrate_project_candidate(raw,r,target_schema,target_graph,marker);
}
struct ProjectSections {
    QJsonValue processing, draft, presentation, presets;
    QJsonObject application;
};
inline ProjectSections project_sections(const QJsonObject &raw) {
    auto application=raw;
    const QStringList keys{"schema_version","pipeline_graph","scene_graph","ui","presets","devices",
                           "_configuration_project_migration"};
    for(const auto &key:keys)application.remove(key);
    const auto ui=raw.value("ui").toObject();
    return {raw.value("pipeline_graph"),raw.value("scene_graph"),raw.value("ui"),
            raw.contains("presets")?raw.value("presets"):ui.value("pipeline_presets"),application};
}
struct SaveResult {
    QString error;
    explicit operator bool() const {return error.isEmpty();}
};
inline SaveResult validate_existing_source(const QString& path) {
    if(!QFileInfo::exists(path))return {};
    QFile source(path);if(!source.open(QIODevice::ReadOnly))return {source.errorString()};
    QJsonParseError parse;const auto old=QJsonDocument::fromJson(source.readAll(),&parse);
    if(parse.error!=QJsonParseError::NoError || !old.isObject())return {"Source configuration is malformed"};
    const auto mapped=migrate(old.object());if(!mapped)return {mapped.error};
    return {};
}
inline SaveResult write_atomic_document(const QString& path,const QJsonObject& document) {
    const auto dir=QFileInfo(path).absoluteDir();if(!dir.exists() && !dir.mkpath("."))return {"Could not create configuration directory"};
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly))return {file.errorString()};
    const auto bytes=QJsonDocument(document).toJson();
    if(file.write(bytes)!=bytes.size()){file.cancelWriting();return {file.errorString()};}
    if(!file.commit())return {file.errorString()};return {};
}
inline SaveResult save_atomic(const QString &path,const QJsonObject &document) {
    const auto mapped=migrate(document);if(!mapped)return {mapped.error_path+": "+mapped.error};
    // A supported candidate must not overwrite a future/malformed source file.
    const auto source=validate_existing_source(path);if(!source)return source;
    return write_atomic_document(path,mapped.document);
}
} // namespace configuration
