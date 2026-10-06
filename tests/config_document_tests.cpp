#include "config/config_document.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSaveFile>
#include <stdexcept>
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QJsonObject graph{{"version", 2}, {"nodes", QJsonArray{QJsonObject{{"id", 1}}}}};
    QJsonObject document{
        {"extension", QJsonObject{{"opaque", 42}}},
        {"pipeline_graph", graph},
        {"ui", QJsonObject{{"pipeline_presets",
                            QJsonArray{QJsonObject{{"id", "one"}, {"name", "Same"}, {"graph", graph}},
                                       QJsonObject{{"id", "two"}, {"name", "Same"}, {"graph", graph}},
                                       QJsonObject{{"id", "broken"}, {"name", "Broken"}, {"graph", 42}}}}}}};
    require(configuration::resolve(graph).graph == graph, "standalone raw v2 graph rejected");
    auto legacy = graph;
    legacy["version"] = 1;
    require(configuration::resolve(legacy).graph == legacy, "standalone raw v1 graph rejected");
    require(!configuration::resolve(graph, "one"), "raw graph accepted missing explicit preset");
    require(bool(configuration::resolve(document)), "default graph not resolved");
    require(bool(configuration::resolve(document, "one")), "preset id not resolved");
    require(!configuration::resolve(document, "missing"), "missing preset silently fell back");
    require(!configuration::resolve(document, "Same"), "ambiguous preset accepted");
    require(!configuration::resolve(document, "broken"), "invalid preset graph accepted");
    auto merged = configuration::overlay(document, {{"ui", QJsonObject{{"language", "en"}}}});
    require(merged["extension"] == document["extension"] &&
                merged["ui"].toObject()["pipeline_presets"] == document["ui"].toObject()["pipeline_presets"],
            "overlay lost unknown fields/presets");
    QSaveFile file(dir.filePath("config.json"));
    require(file.open(QIODevice::WriteOnly), "cannot write");
    file.write(QJsonDocument(merged).toJson());
    require(file.commit(), "cannot commit");
    auto loaded = configuration::load(file.fileName(), "two");
    require(bool(loaded) && loaded.graph == graph && loaded.document == merged, "roundtrip changed document");
    document["pipeline_graph"] = 42;
    document["scene_graph"] = graph;
    require(!configuration::resolve(document), "malformed explicit graph silently fell back");
    qInfo("Core-only resolver: missing/ambiguous/invalid presets and unknown-field roundtrip passed");
}
