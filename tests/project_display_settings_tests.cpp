#include <desktop/project_display_settings.h>
#include <QJsonArray>
#include <cassert>
#include <limits>

int main() {
    const QJsonObject local{{"graph_id","project"},{"version",2},{"extension",42},
        {"nodes",QJsonArray{
            QJsonObject{{"id",7},{"position",QJsonObject{{"x",12}}},{"internal-data",QJsonObject{
                {"model-name","FFTView"},{"channel","future-selector"},{"merge",2},
                {"renderer",QJsonObject{{"color","local"}}},{"opaque",QJsonObject{{"gain",999}}}}}},
            QJsonObject{{"id",8},{"internal-data",QJsonObject{{"model-name","SpectrumGain"},{"gain",2.5},
                {"channel",1},{"opaque",QJsonObject{{"position",true}}}}}}}},
        {"connections",QJsonArray{}}};
    const auto input=desktop::project_processing_dto(local);
    assert(input && input.processing.graph.graph_id=="project");
    assert(std::holds_alternative<domain::NoParameters>(input.processing.graph.nodes[0].parameters));
    assert(std::get<domain::GainParameters>(input.processing.graph.nodes[1].parameters).gain==2.5);
    const auto wire=desktop::processing_dto_document(input.processing);
    assert(wire && !wire.document.contains("extension"));
    for(const auto &value:wire.document["nodes"].toArray()) {
        const auto node=value.toObject(), p=node["internal-data"].toObject();
        assert(!node.contains("position") && !p.contains("opaque") && !p.contains("channel") && !p.contains("renderer"));
    }
    const auto again=desktop::project_processing_dto(wire.document);
    assert(again && again.processing.graph==input.processing.graph);
    const auto settings=desktop::capture_project_display_settings(local);
    assert(settings);
    auto accepted=input.processing;
    std::get<domain::GainParameters>(accepted.graph.nodes[1].parameters).gain=3;
    const auto restored=desktop::merge_project_display_settings(accepted,settings.settings);
    assert(restored && restored.document["extension"]==42);
    const auto nodes=restored.document["nodes"].toArray();
    assert(nodes[0].toObject()["position"]==local["nodes"].toArray()[0].toObject()["position"]);
    assert(nodes[0].toObject()["internal-data"].toObject()["channel"]=="future-selector");
    assert(nodes[0].toObject()["internal-data"].toObject()["opaque"].isObject());
    assert(nodes[1].toObject()["internal-data"].toObject()["gain"]==3);
    assert(desktop::project_processing_dto(restored.document).processing.graph==accepted.graph);
    accepted.graph.graph_id="foreign";
    const auto foreign=desktop::merge_project_display_settings(accepted,settings.settings);
    assert(foreign && foreign.document==desktop::processing_dto_document(accepted).document);
    accepted=input.processing;
    accepted.graph.nodes[0]={7,domain::NodeKind::MelView,domain::NoParameters{},true};
    const auto reused=desktop::merge_project_display_settings(accepted,settings.settings);
    assert(reused && !reused.document["nodes"].toArray()[0].toObject().contains("position"));
    accepted=input.processing;accepted.graph.nodes.erase(accepted.graph.nodes.begin());
    assert(desktop::merge_project_display_settings(accepted,settings.settings).document["nodes"].toArray().size()==1);
    auto wired=local;wired["connections"]=QJsonArray{QJsonObject{
        {"outNodeId",8},{"inNodeId",7},{"outPortIndex",0},{"inPortIndex",0},{"local-edge-extension",42}}};
    auto wiredDTO=input.processing;wiredDTO.graph.edges.push_back({8,7,0,0});
    const auto wiredSettings=desktop::capture_project_display_settings(wired);assert(wiredSettings);
    const auto mergedEdge=desktop::merge_project_display_settings(wiredDTO,wiredSettings.settings);
    assert(mergedEdge && mergedEdge.document["connections"].toArray()[0].toObject()["local-edge-extension"]==42);
    assert(!desktop::processing_dto_document(wiredDTO).document["connections"].toArray()[0].toObject().contains("local-edge-extension"));
    wiredDTO.graph.edges[0].output_port=1;
    assert(!desktop::merge_project_display_settings(wiredDTO,wiredSettings.settings).document["connections"].toArray()[0].toObject().contains("local-edge-extension"));
    auto duplicate=local;auto duplicated=local["nodes"].toArray();duplicated.append(duplicated[0]);duplicate["nodes"]=duplicated;
    assert(!desktop::capture_project_display_settings(duplicate));
    assert(!desktop::project_processing_dto(duplicate));
    auto invalid=local;invalid["processing_version"]=2;assert(!desktop::project_processing_dto(invalid));
    invalid=local;invalid["schema_version"]=2;
    assert(!desktop::project_processing_dto(invalid) && !desktop::capture_project_display_settings(invalid));
    accepted=input.processing;accepted.graph.processing_version=2;assert(!desktop::processing_dto_document(accepted));
    accepted=input.processing;accepted.graph.nodes[1].parameters=domain::GainParameters{std::numeric_limits<double>::infinity()};
    assert(!desktop::processing_dto_document(accepted));
    struct InvalidParameter {const char* kind;const char* key;QJsonValue value;};
    const InvalidParameter invalid_parameters[]{
        {"AudioSource","source",99},{"SpectrumMerge","merge",99},{"SpectrumGain","gain",-1},
        {"Mel","bands",51},{"Spectrogram","history_size",1},{"Window","size",513},
        {"TestSignal","tones",11},{"TestSignal","sample_rate",1},{"TestSignal","amplitude",2},
        {"TestSignal","period_seconds",0},{"ScalarReduce","operation",99},
        {"NetworkInput","port",0},{"NetworkInput","stream_id","   "}};
    for(const auto& example:invalid_parameters) {
        QJsonObject parameters{{"model-name",example.kind}};
        if(QString(example.kind)=="NetworkInput")parameters["stream_id"]="s";
        parameters[example.key]=example.value;
        const QJsonObject draft{{"version",2},{"processing_version",1},{"graph_id","invalid-draft"},
            {"nodes",QJsonArray{QJsonObject{{"id",0},{"internal-data",parameters}}}},{"connections",QJsonArray{}}};
        const auto mapped=desktop::read_processing_graph_document(draft);
        assert(mapped.error.isEmpty() && !domain::parameters_valid(mapped.graph.nodes[0]));
        const auto rejected=desktop::project_processing_dto(draft);
        assert(!rejected && rejected.processing.graph.nodes.empty());
        assert(!desktop::processing_dto_document({mapped.graph}));
        const auto captured=desktop::capture_project_display_settings(draft);
        assert(captured && captured.settings.document()==draft); // A draft is local, not processing admission.
    }
    for(const auto* kind:{"NetworkInput","NetworkOutput"}) {
        for(bool canonical:{false,true}) {
            QJsonObject parameters{{"model-name",kind},{"stream_id","s"},{"type","spectrum"},
                {"opaque",QJsonObject{{"channel",99}}},{"provider_uuid","u"},{"enabled",true}};
            if(canonical)parameters["data_type"]="mel";
            const QJsonObject legacy{{"version",2},{"nodes",QJsonArray{
                QJsonObject{{"id",0},{"internal-data",parameters}}}},{"connections",QJsonArray{}}};
            const auto typed=desktop::project_processing_dto(legacy);assert(typed);
            assert(std::get<domain::NetworkParameters>(typed.processing.graph.nodes[0].parameters).data_type==
                (canonical?signal_contracts::FrameType::Mel:signal_contracts::FrameType::Spectrum));
            const auto captured=desktop::capture_project_display_settings(legacy);assert(captured);
            const auto merged=desktop::merge_project_display_settings(typed.processing,captured.settings);assert(merged);
            const auto preserved=merged.document["nodes"].toArray()[0].toObject()["internal-data"].toObject();
            assert(preserved["type"]=="spectrum" && preserved["opaque"]==parameters["opaque"]);
            assert(legacy["nodes"].toArray()[0].toObject()["internal-data"]==parameters);
        }
    }
    // Every registered variant is exercised, including adapter network strings,
    // enum IDs and configured routing membership. UI parameters stay absent.
    for(const auto& descriptor:domain::node_descriptors()) {
        application::ProcessingDTO dto;
        auto params=domain::default_parameters(descriptor.kind);
        if(auto* network=std::get_if<domain::NetworkParameters>(&params)) {
            network->stream_id="identity";network->provider_uuid="uuid";
            network->host="host";network->port=4321;
        }
        dto.graph.nodes.push_back({0,descriptor.kind,params,false});
        const auto encoded=desktop::processing_dto_document(dto);assert(encoded);
        const auto decoded=desktop::project_processing_dto(encoded.document);assert(decoded);
        auto expected=dto.graph;
        expected.nodes[0]=domain::project_processing_node(expected.nodes[0]);
        assert(decoded.processing.graph==expected);
        const auto fields=encoded.document["nodes"].toArray()[0].toObject()["internal-data"].toObject();
        for(auto it=fields.begin();it!=fields.end();++it)if(it.key()!="model-name") {
            const auto* field=domain::parameter_descriptor(descriptor.kind,it.key().toStdString());
            assert(field && field->classification!=domain::ParameterClassification::Presentation &&
                field->classification!=domain::ParameterClassification::OpaqueExtension);
        }
    }
}
