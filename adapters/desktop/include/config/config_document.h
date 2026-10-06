#pragma once
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
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
inline Result resolve(QJsonObject document, const QString &preset = {}) {
    Result result;
    result.document = document;
    if (preset.isEmpty()) {
        if (document.value("nodes").isArray()) {
            result.graph = document;
            return result;
        }
        const auto key = document.contains("pipeline_graph") ? "pipeline_graph" : "scene_graph";
        if (!document.value(key).isObject())
            result.error = "Configuration has no object processing graph";
        else
            result.graph = document.value(key).toObject();
        return result;
    }
    const auto presets = document.contains("presets")
                             ? document.value("presets").toArray()
                             : document.value("ui").toObject().value("pipeline_presets").toArray();
    QJsonObject selected;
    int matches = 0;
    // IDs take precedence over names; duplicate IDs are also rejected.
    for (const auto &value : presets)
        if (value.toObject().value("id").toString() == preset) {
            selected = value.toObject();
            ++matches;
        }
    if (!matches)
        for (const auto &value : presets)
            if (value.toObject().value("name").toString() == preset) {
                selected = value.toObject();
                ++matches;
            }
    if (matches != 1)
        result.error = matches ? "Preset selection is ambiguous: " + preset : "Preset not found: " + preset;
    else if (!selected.value("graph").isObject())
        result.error = "Preset has no object graph: " + preset;
    else {
        result.graph = selected.value("graph").toObject();
        result.preset_id = selected.value("id").toString();
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
    result.document = parsed.object();
    return result;
}
inline Result load(const QString &path, const QString &preset = {}) {
    auto result = read(path);
    return result ? resolve(result.document, preset) : result;
}
} // namespace configuration
