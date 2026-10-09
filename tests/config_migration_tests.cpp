#include <config/config_document.h>
#include <config/config_migration.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>
#ifdef CONFIGURATION_MANAGER_TESTS
#include <core_app/config_manager.h>
#endif

static QJsonObject graph() {
    return {{"version",2},{"graph_id","stable"},{"nodes",QJsonArray{}},{"connections",QJsonArray{}},
            {"unknown",QJsonArray{1,QJsonObject{{"nested",true}}}}};
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const auto g=graph();
    const QJsonObject doc{{"pipeline_graph",g},{"scene_graph",42},{"ui",QJsonObject{
        {"camera",QJsonArray{1,2}}, {"pipeline_presets",QJsonArray{
            QJsonObject{{"id","same"},{"name","first"},{"graph",g},{"opaque",false}},
            QJsonObject{{"id","other"},{"name","same"},{"graph",QJsonObject{{"nodes",QJsonArray{}}}}}}}}}};
    assert(configuration::resolve(doc).graph==g);
    assert(configuration::resolve(doc,"same").graph==g); // ID wins over name.
    assert(configuration::resolve(g).graph==g);
    auto bad=doc;bad["pipeline_graph"]=false;
    assert(!configuration::resolve(bad)); // Explicit malformed applied never falls back.
    const auto over=configuration::overlay(doc,{{"ui",QJsonObject{{"language","en"}}}});
    assert(over["scene_graph"]==42 && over["pipeline_graph"]==g);
    assert(over["ui"].toObject()["camera"]==doc["ui"].toObject()["camera"]);
    assert(over["ui"].toObject()["pipeline_presets"]==doc["ui"].toObject()["pipeline_presets"]);
    auto migrated=configuration::migrate(doc);
    assert(migrated && migrated.document["schema_version"]==1);
    assert(migrated.document["scene_graph"]==42);
    assert(migrated.document["pipeline_graph"].toObject()["processing_version"]==1);
    assert(configuration::migrate(migrated.document,0).document==doc);
    for (const QJsonValue version: {QJsonValue(),QJsonValue("1"),QJsonValue(0),QJsonValue(-1),QJsonValue(1.5),QJsonValue(2)}) {
        auto invalid=doc;invalid["schema_version"]=version;
        const auto failed=configuration::migrate(invalid);
        assert(!failed && failed.document==invalid);
        assert(!configuration::resolve(invalid));
    }
    QTemporaryDir dir;
    const auto path=dir.filePath("config.json");
    assert(configuration::save_atomic(path,doc));
    QFile file(path);assert(file.open(QIODevice::ReadOnly));const auto before=file.readAll();file.close();
    auto future=doc;future["schema_version"]=2;
    assert(!configuration::save_atomic(path,future));
    assert(file.open(QIODevice::ReadOnly));assert(file.readAll()==before);file.close();
    assert(configuration::read(path).document["schema_version"]==1);
    auto standalone=configuration::migrate(g);
    assert(standalone && standalone.document["scene_graph"]==standalone.document["pipeline_graph"]);
    assert(configuration::migrate(standalone.document,0).document==g);
    QJsonObject v1{{"version",1},{"graph_id","old"},{"nodes",QJsonArray{
        QJsonObject{{"id",4},{"internal-data",QJsonObject{{"model-name","AudioSource"}}}},
        QJsonObject{{"id",8},{"internal-data",QJsonObject{{"model-name","StereoFFT"}}}},
        QJsonObject{{"id",12},{"internal-data",QJsonObject{{"model-name","SurfaceView"},{"custom",QJsonArray{false,3}}}}}}},
        {"connections",QJsonArray{QJsonObject{{"outNodeId",4},{"inNodeId",8},{"outPortIndex",0},{"inPortIndex",0},{"opaque",99}}}}};
    auto up=configuration::migrate_graph(v1,2);
    assert(up && up.document["version"]==2 && up.document["nodes"].toArray().size()==6);
    assert(up.document["nodes"].toArray()[0].toObject()["id"]==4);
    assert(configuration::migrate_graph(up.document,1).document==v1);
    auto edited=up.document;auto nodes=edited["nodes"].toArray();auto node=nodes[3].toObject();
    auto params=node["internal-data"].toObject();params["size"]=512;node["internal-data"]=params;nodes[3]=node;edited["nodes"]=nodes;
    assert(!configuration::migrate_graph(edited,1));
    assert(!configuration::migrate_graph(g,1)); // Native v2 has no proved inverse.
    auto broken=v1;broken["version"]="1";assert(!configuration::migrate_graph(broken,2));
    assert(configuration::migrate(migrated.document).document==migrated.document);
    const auto futurePath=dir.filePath("future.json");
    QFile futureFile(futurePath);assert(futureFile.open(QIODevice::WriteOnly));
    const auto futureBytes=QJsonDocument(future).toJson();assert(futureFile.write(futureBytes)==futureBytes.size());futureFile.close();
    assert(!configuration::save_atomic(futurePath,doc));
    assert(futureFile.open(QIODevice::ReadOnly));assert(futureFile.readAll()==futureBytes);futureFile.close();
    auto invalidVersion=g;invalidVersion["version"]=2.5;assert(!configuration::resolve(invalidVersion));
    const auto sections=configuration::project_sections(doc);
    assert(sections.processing==g && sections.draft==42 && sections.presets.isArray());
    auto devices=doc;devices["devices"]=QJsonObject{{"uuid",QJsonObject{{"pipeline_graph",g},{"scene_graph",false},{"opaque",QJsonArray{1,2}}}}};
    const auto deviceMigration=configuration::migrate(devices);
    assert(deviceMigration && deviceMigration.document["devices"].toObject()["uuid"].toObject()["opaque"].toArray()==QJsonArray({1,2}));
    assert(deviceMigration.document["devices"].toObject()["uuid"].toObject()["scene_graph"]==false);
    auto device=devices["devices"].toObject()["uuid"].toObject();auto futureGraph=g;futureGraph["processing_version"]=2;
    device["pipeline_graph"]=futureGraph;devices["devices"]=QJsonObject{{"uuid",device}};
    assert(!configuration::migrate(devices));
    auto exhausted=v1;auto exhaustedNodes=exhausted["nodes"].toArray();auto exhaustedNode=exhaustedNodes[0].toObject();
    exhaustedNode["id"]=2147483647;exhaustedNodes[0]=exhaustedNode;exhausted["nodes"]=exhaustedNodes;
    const auto refused=configuration::migrate_graph(exhausted);assert(!refused && refused.document==exhausted);
    const QJsonObject minimal{{"schema_version",1},{"pipeline_graph",v1}};
    const auto minimal_up=configuration::migrate(minimal,1,2);
    const auto minimal_down=configuration::migrate(minimal_up.document,1,1);
    assert(minimal_up && minimal_down && minimal_down.document==minimal);
    assert(!minimal_up.document.contains("presets") && !minimal_up.document.contains("ui"));
    const QJsonObject uiOnly{{"schema_version",1},{"ui",QJsonObject{{"pipeline_presets",QJsonArray{
        QJsonObject{{"id","ui-only"},{"graph",g},{"opaque",true}}}}}}};
    const auto uiPath=dir.filePath("ui-only.json");assert(configuration::save_atomic(uiPath,uiOnly));
    const auto uiSaved=configuration::read(uiPath);assert(uiSaved && !uiSaved.document.contains("presets"));
    assert(configuration::resolve(uiSaved.document,"ui-only"));
    auto explicitNull=uiOnly;explicitNull["presets"]=QJsonValue();
    const auto nullMapped=configuration::migrate(explicitNull);
    assert(nullMapped && nullMapped.document.contains("presets") && nullMapped.document.value("presets").isNull());
    // A present malformed root preset list is authoritative, never silently
    // interpreted as absence; the historical resolver refuses UI fallback.
    assert(!configuration::resolve(explicitNull,"ui-only"));
    for(bool explicit_processing:{false,true}) {
        auto legacy_graph=v1;if(explicit_processing)legacy_graph["processing_version"]=1;
        const QJsonObject preset{{"id","preserved"},{"name","Legacy"},{"graph",legacy_graph},{"opaque",QJsonArray{false,7}}};
        const QJsonObject chain{{"schema_version",1},{"pipeline_graph",legacy_graph},{"scene_graph",legacy_graph},
            {"presets",QJsonArray{preset}}, {"ui",QJsonObject{{"pipeline_presets",QJsonArray{preset}}}},
            {"devices",QJsonObject{{"remote",QJsonObject{{"pipeline_graph",legacy_graph},{"scene_graph",legacy_graph},{"presets",QJsonArray{preset,42}}}}}}};
        const auto up_chain=configuration::migrate(chain,1,2);assert(up_chain);
        const auto down_chain=configuration::migrate(up_chain.document,1,1);
        assert(down_chain && down_chain.document==chain);
        auto edited_chain=up_chain.document;auto graph_edit=edited_chain["pipeline_graph"].toObject();
        graph_edit["custom-edit"]=true;edited_chain["pipeline_graph"]=graph_edit;
        assert(!configuration::migrate(edited_chain,1,1));
    }
    for(const auto key:QStringList{"version","processing_version"})for(const QJsonValue badVersion:{QJsonValue(),QJsonValue("1"),QJsonValue(99)}) {
        auto badGraph=g;badGraph[key]=badVersion;
        const QJsonObject preset{{"id","protected-preset"},{"graph",badGraph},{"unknown",QJsonArray{true,8}}};
        const QJsonObject badDevice{{"schema_version",1},{"devices",QJsonObject{{"remote",QJsonObject{{"presets",QJsonArray{42,preset}}}}}}};
        const auto badMigration=configuration::migrate(badDevice);assert(!badMigration && badMigration.document==badDevice);
        assert(badMigration.error_path.startsWith("devices/remote/presets/1/graph/"));
        const auto protectedPath=dir.filePath("device-preset.json");QFile protectedFile(protectedPath);
        const auto bytes=QJsonDocument(badDevice).toJson();assert(protectedFile.open(QIODevice::WriteOnly));protectedFile.write(bytes);protectedFile.close();
        assert(!configuration::save_atomic(protectedPath,{{"schema_version",1},{"sentinel","replacement"}}));
        assert(protectedFile.open(QIODevice::ReadOnly));assert(protectedFile.readAll()==bytes);protectedFile.close();
    }
#ifdef CONFIGURATION_MANAGER_TESTS
    const auto managerPath=dir.filePath("manager.json");
    ConfigManager uiManager(uiPath);FileSaveApplicationConfig uiConfig;
    assert(uiManager.load(uiConfig));uiConfig.ui.language="en";assert(uiManager.save(uiConfig));
    const auto uiPrefs=configuration::read(uiPath);
    assert(uiPrefs && !uiPrefs.document.contains("presets") && configuration::resolve(uiPrefs.document,"ui-only"));
    auto stored=doc;stored["pipeline_graph"]=42;
    stored["ui"]=configuration::overlay(stored["ui"].toObject(),{{"theme","future-theme"},{"camera_controls",42}});
    assert(configuration::save_atomic(managerPath,stored));
    ConfigManager manager(managerPath);FileSaveApplicationConfig config;QString error;
    assert(manager.load(config,&error));
    config.ui.language="en";assert(manager.save(config,&error));
    const auto saved=configuration::read(managerPath);
    assert(saved && saved.document["pipeline_graph"]==42 && saved.document["scene_graph"]==42);
    assert(saved.document["ui"].toObject()["theme"]=="future-theme");
    assert(saved.document["ui"].toObject()["camera_controls"]==42);
    assert(saved.document["ui"].toObject()["language"]=="en");
    const auto active=manager.getConfigPath();assert(!manager.saveAs(config,futurePath,&error));
    assert(manager.getConfigPath()==active);
    FileSaveApplicationConfig unchanged;unchanged.raw_document={{"sentinel",true}};
    assert(!ConfigManager(futurePath).load(unchanged,&error));assert(unchanged.raw_document["sentinel"]==true);
    assert(ConfigManager().getConfigPath()==configuration::active_path());
    const auto standalonePath=dir.filePath("standalone.json");
    QFile standaloneFile(standalonePath);assert(standaloneFile.open(QIODevice::WriteOnly));
    standaloneFile.write(QJsonDocument(g).toJson());standaloneFile.close();
    ConfigManager standaloneManager(standalonePath);FileSaveApplicationConfig standaloneConfig;
    assert(standaloneManager.load(standaloneConfig));standaloneConfig.pipeline_graph["graph_id"]="edited";
    assert(standaloneManager.save(standaloneConfig));
    assert(configuration::read(standalonePath).document["pipeline_graph"].toObject()["graph_id"]=="edited");
#endif
}
