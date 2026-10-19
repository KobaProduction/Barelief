#pragma once
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <utility>
#include <config/config_migration.h>
namespace configuration {
struct Result {
    QJsonObject document, graph;
    QString error, preset_id;
    explicit operator bool() const {
        return error.isEmpty();
    }
};
inline QJsonObject overlay(QJsonObject original, const QJsonObject &updates) {
    for (auto it = updates.begin(); it != updates.end(); ++it)
        original[it.key()] = it.value().isObject() && original.value(it.key()).isObject()
                                 ? QJsonValue(overlay(original.value(it.key()).toObject(), it.value().toObject()))
                                 : it.value();
    return original;
}
inline void check_resolved_graph(Result& result) {
    result.error=check_version(result.graph,"version",2);
    if(result.error.isEmpty())result.error=check_version(result.graph,"processing_version",1);
}
inline void resolve_default_graph(Result& result) {
    const auto& document=result.document;
    if(document.value("nodes").isArray())result.graph=document;
    else {
        const auto key=document.contains("pipeline_graph")?"pipeline_graph":"scene_graph";
        if(!document.value(key).isObject())result.error="Configuration has no object processing graph";
        else result.graph=document.value(key).toObject();
    }
    if(result.error.isEmpty())check_resolved_graph(result);
}
inline std::pair<QJsonObject,int> find_preset_field(const QJsonArray& presets,const QString& preset,const char* field) {
    QJsonObject selected;
    int matches=0;
    for(const auto& value:presets) {
        if(value.toObject().value(field).toString()!=preset)continue;
        selected=value.toObject();
        ++matches;
    }
    return {selected,matches};
}
inline std::pair<QJsonObject,int> find_preset(const QJsonArray& presets,const QString& preset) {
    // IDs take precedence over names; duplicate IDs are also rejected.
    auto selected=find_preset_field(presets,preset,"id");
    return selected.second?selected:find_preset_field(presets,preset,"name");
}
inline void resolve_preset_graph(Result& result,const QString& preset) {
    const auto& document=result.document;
    const auto presets = document.contains("presets")
                             ? document.value("presets").toArray()
                             : document.value("ui").toObject().value("pipeline_presets").toArray();
    const auto [selected,matches]=find_preset(presets,preset);
    if (matches != 1)
        result.error = matches ? "Preset selection is ambiguous: " + preset : "Preset not found: " + preset;
    else if (!selected.value("graph").isObject())
        result.error = "Preset has no object graph: " + preset;
    else {
        result.graph = selected.value("graph").toObject();
        result.preset_id = selected.value("id").toString();
        check_resolved_graph(result);
    }
}
inline Result resolve(QJsonObject document, const QString &preset = {}) {
    Result result;
    result.document=std::move(document);
    const auto classification=classify_document(result.document);
    if(!classification){result.error=classification.error;return result;}
    if(preset.isEmpty())resolve_default_graph(result);
    else resolve_preset_graph(result,preset);
    return result;
}
// Calculate only edits relative to a parsed snapshot. Unchanged defaults do not
// replace an explicit raw value which the compatibility reader normalized.
inline QJsonObject changed_fields(const QJsonObject &before,const QJsonObject &after) {
    QJsonObject result;
    for(auto it=after.begin();it!=after.end();++it) {
        if(before.value(it.key())==it.value())continue;
        if(before.value(it.key()).isObject() && it.value().isObject())
            result[it.key()]=changed_fields(before.value(it.key()).toObject(),it.value().toObject());
        else result[it.key()]=it.value();
    }
    return result;
}
inline Result read(const QString &path) {
    QFile file(path);
    Result result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !parsed.isObject()) {
        result.error =
            error.error == QJsonParseError::NoError ? "Configuration must be an object" : error.errorString();
        return result;
    }
    const auto mapped=migrate(parsed.object());
    result.document = parsed.object(); // Raw storage is distinct from migrated projections.
    if(!mapped)result.error=mapped.error_path+": "+mapped.error;
    return result;
}
inline Result load(const QString &path, const QString &preset = {}) {
    auto result = read(path);
    return result ? resolve(result.document, preset) : result;
}
} // namespace configuration
