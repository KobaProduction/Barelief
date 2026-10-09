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
inline MigrationResult migrate_graph(const QJsonObject &raw,int target=2) {
    MigrationResult r;r.document=raw;r.target_version=target;
    r.error=check_version(raw,"version",2);r.error_path="version";
    if(r) {r.error=check_version(raw,"processing_version",1);r.error_path="processing_version";}
    if(!r)return r;
    r.source_version=2; // Historical unversioned graph is the existing v2 shape.
    if(raw.contains("version"))version_number(raw["version"],r.source_version);
    if(target!=1 && target!=2){r.error="Unsupported target graph version";return r;}
    if(r.source_version==target)return r;
    const QString marker="_configuration_graph_migration";
    if(target==1) {
        const auto record=raw.value(marker).toObject();auto current=raw;current.remove(marker);
        if(record.value("source").isObject() && record.value("mapped")==current) {
            r.document=record.value("source").toObject();return r;
        }
        r.error="Unsupported or lossy graph downgrade";r.error_path=marker;return r;
    }
    if(raw.contains(marker)){r.error="Migration extension collision";r.error_path=marker;return r;}
    if(!raw.value("nodes").isArray() || !raw.value("connections").isArray()) {
        r.error="Legacy graph arrays missing";r.error_path="nodes/connections";return r;
    }
    auto nodes=raw["nodes"].toArray();auto edges=raw["connections"].toArray();
    QMap<int,QString> kinds;int next=0;
    for(int i=0;i<nodes.size();++i) {
        auto n=nodes[i].toObject();const auto id=n.value("id");
        // Node zero is valid; version_number intentionally is not reused.
        const double d=id.toDouble(-1);
        if(!id.isDouble() || d<0 || d>=std::numeric_limits<int>::max()-3 || std::floor(d)!=d || kinds.contains(int(d))) {
            r.error="Invalid legacy node ID or ID space exhausted";r.error_path="nodes/id";return r;
        }
        next=std::max(next,int(d)+1);auto p=n["internal-data"].toObject();
        if(p["model-name"]=="SurfaceView")p["model-name"]="SpectrogramView";
        kinds[int(d)]=p["model-name"].toString();n["internal-data"]=p;nodes[i]=n;
    }
    const auto old_edges=edges;
    auto add=[&](const QString &kind,const QJsonObject &near) {
        const int id=next++;nodes.append(QJsonObject{{"id",id},{"position",near.value("position").toObject()},
            {"internal-data",QJsonObject{{"model-name",kind}}}});return id;
    };
    auto edge=[](int from,int to){return QJsonObject{{"outNodeId",from},{"inNodeId",to},{"outPortIndex",0},{"inPortIndex",0}};};
    // Check worst-case added nodes before assigning a single ID.
    if(qint64(next)+qint64(edges.size())*2+kinds.size()>=std::numeric_limits<int>::max()) {
        r.error="Legacy node ID space exhausted";r.error_path="nodes/id";return r;
    }
    for(int i=0;i<old_edges.size();++i) {
        auto e=old_edges[i].toObject();const auto f=e["outNodeId"],t=e["inNodeId"];
        const double fd=f.toDouble(-1),td=t.toDouble(-1);
        if(!f.isDouble() || !t.isDouble() || fd<0 || td<0 || fd>std::numeric_limits<int>::max() || td>std::numeric_limits<int>::max() ||
           std::floor(fd)!=fd || std::floor(td)!=td || !kinds.contains(int(fd)) || !kinds.contains(int(td))) {
            r.error="Invalid legacy edge identity";r.error_path="connections";return r;
        }
        if(kinds[int(td)]!="StereoFFT" || (kinds[int(fd)]!="AudioSource" && kinds[int(fd)]!="TestSignal"))continue;
        if(e["outPortIndex"]!=0 || e["inPortIndex"]!=0){r.error="Unsupported legacy edge port";r.error_path="connections";return r;}
        QJsonObject near;for(const auto &n:nodes)if(n.toObject()["id"]==t)near=n.toObject();
        const int w=add("Window",near),h=add("WindowFunction",near);
        e["inNodeId"]=w;edges[i]=e;edges.append(edge(w,h));edges.append(edge(h,int(td)));
    }
    for(auto it=kinds.begin();it!=kinds.end();++it)if(it.value()=="StereoFFT") {
        const int bark=add("BarkWeighting",{});
        for(int i=0;i<edges.size();++i){auto e=edges[i].toObject();if(e["outNodeId"]==it.key()){e["outNodeId"]=bark;edges[i]=e;}}
        edges.append(edge(it.key(),bark));
    }
    auto mapped=raw;mapped["version"]=2;mapped["nodes"]=nodes;mapped["connections"]=edges;
    // The recorded output includes every field introduced by this step. A
    // missing legacy processing version is restored by the exact inverse.
    if(!mapped.contains("processing_version"))mapped["processing_version"]=1;
    r.document=mapped;r.document[marker]=QJsonObject{{"source",raw},{"mapped",mapped}};return r;
}

inline MigrationResult migrate(const QJsonObject &raw,int target_schema=1,int target_graph=2) {
    auto r=classify_document(raw);r.target_version=target_schema;if(!r)return r;
    if(target_schema!=0 && target_schema!=1){r.error="Unsupported target schema";return r;}
    const QString marker="_configuration_project_migration";
    if(target_schema==0 && r.source_version==1) {
        const auto record=raw.value(marker).toObject();auto current=raw;current.remove(marker);
        if(record.value("source").isObject() && record.value("mapped")==current) {
            r.document=record["source"].toObject();return r;
        }
        r.error="Unsupported or lossy project downgrade";r.error_path=marker;return r;
    }
    auto candidate=raw;
    const bool standalone=raw.value("nodes").isArray();
    if(standalone) {
        candidate.remove("nodes");candidate.remove("connections");candidate.remove("version");candidate.remove("processing_version");
        candidate["pipeline_graph"]=raw;candidate["scene_graph"]=raw;
    } else if(!raw.contains("pipeline_graph") && raw.value("scene_graph").isObject())candidate["pipeline_graph"]=raw["scene_graph"];
    auto map_graph=[&](QJsonValue &value,const QString &path)->bool {
        if(!value.isObject())return true; // Malformed draft/value remains opaque until explicitly replaced.
        auto g=value.toObject();auto mapped=migrate_graph(g,target_graph);
        if(!mapped){r.error=mapped.error;r.error_path=path+"/"+mapped.error_path;return false;}
        g=mapped.document;
        if(target_graph==2 && !g.contains("processing_version"))g["processing_version"]=1;
        value=g;return true;
    };
    for(const auto &key:QStringList{"pipeline_graph","scene_graph"})if(candidate.contains(key)) {
        QJsonValue v=candidate[key];if(!map_graph(v,key))return r;candidate[key]=v;
    }
    auto map_presets=[&](QJsonArray &presets,const QString &path)->bool {
        for(int i=0;i<presets.size();++i){auto p=presets[i].toObject();if(!p.contains("graph"))continue;
            QJsonValue v=p["graph"];if(!map_graph(v,path+"/"+QString::number(i)+"/graph"))return false;p["graph"]=v;presets[i]=p;}return true;
    };
    if(candidate.value("presets").isArray()){auto a=candidate.value("presets").toArray();if(!map_presets(a,"presets"))return r;candidate["presets"]=a;}
    auto ui=candidate.value("ui").toObject();
    if(ui.value("pipeline_presets").isArray()){auto a=ui.value("pipeline_presets").toArray();if(!map_presets(a,"ui/pipeline_presets"))return r;ui["pipeline_presets"]=a;candidate["ui"]=ui;}
    if(candidate.value("devices").isObject()) {
        auto devices=candidate.value("devices").toObject();
        for(auto it=devices.begin();it!=devices.end();++it) {
            if(!it.value().isObject())continue;
            auto device=it.value().toObject();
            for(const auto &key:QStringList{"pipeline_graph","scene_graph"})if(device.contains(key)) {
                QJsonValue v=device[key];if(!map_graph(v,"devices/"+it.key()+"/"+key))return r;device[key]=v;
            }
            if(device.value("presets").isArray()) {
                auto presets=device.value("presets").toArray();
                if(!map_presets(presets,"devices/"+it.key()+"/presets"))return r;
                device["presets"]=presets;
            }
            it.value()=device;
        }
        candidate["devices"]=devices;
    }
    if(target_schema==0){r.document=candidate;return r;}
    candidate["schema_version"]=1;
    if(r.source_version==0) {
        if(raw.contains(marker)){r.error="Migration extension collision";r.error_path=marker;return r;}
        r.document=candidate;r.document[marker]=QJsonObject{{"source",raw},{"mapped",candidate}};
    } else r.document=candidate;
    return r;
}
struct ProjectSections {
    QJsonValue processing, draft, presentation, presets;
    QJsonObject application;
};
inline ProjectSections project_sections(const QJsonObject &raw) {
    auto application=raw;
    for(const auto &key:QStringList{"schema_version","pipeline_graph","scene_graph","ui","presets","devices",
                                   "_configuration_project_migration"})application.remove(key);
    const auto ui=raw.value("ui").toObject();
    return {raw.value("pipeline_graph"),raw.value("scene_graph"),raw.value("ui"),
            raw.contains("presets")?raw.value("presets"):ui.value("pipeline_presets"),application};
}
struct SaveResult {
    QString error;
    explicit operator bool() const {return error.isEmpty();}
};
inline SaveResult save_atomic(const QString &path,const QJsonObject &document) {
    const auto mapped=migrate(document);if(!mapped)return {mapped.error_path+": "+mapped.error};
    // A supported candidate must not overwrite a future/malformed source file.
    if(QFileInfo::exists(path)) {
        QFile source(path);if(!source.open(QIODevice::ReadOnly))return {source.errorString()};
        QJsonParseError parse;const auto old=QJsonDocument::fromJson(source.readAll(),&parse);
        if(parse.error!=QJsonParseError::NoError || !old.isObject())return {"Source configuration is malformed"};
        const auto old_mapped=migrate(old.object());if(!old_mapped)return {old_mapped.error};
    }
    const auto dir=QFileInfo(path).absoluteDir();if(!dir.exists() && !dir.mkpath("."))return {"Could not create configuration directory"};
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly))return {file.errorString()};
    const auto bytes=QJsonDocument(mapped.document).toJson();
    if(file.write(bytes)!=bytes.size()){file.cancelWriting();return {file.errorString()};}
    if(!file.commit())return {file.errorString()};return {};
}
} // namespace configuration
