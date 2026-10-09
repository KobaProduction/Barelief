#include <desktop/graph_document.h>
#include <QJsonArray>
#include <cassert>
int main() {
    const QJsonObject document{{"graph_id","kept"},{"extension",42},
        {"nodes",QJsonArray{QJsonObject{{"id",7},{"position",QJsonObject{{"x",12}}},
            {"internal-data",QJsonObject{{"model-name","SpectrumGain"},{"gain",2.5},{"extension","keep"}}}}}},
        {"connections",QJsonArray{}}};
    auto result=desktop::read_graph_document(document);
    assert(result.error.isEmpty());
    assert(result.graph.graph_id=="kept" && result.graph.nodes.size()==1);
    assert(result.graph.nodes.front().id==7);
    assert(std::get<domain::GainParameters>(result.graph.nodes.front().parameters).gain==2.5);
    assert(result.document==document); // Unknown fields and layout survive adapter.
    const auto core=desktop::processing_graph_document(document);
    assert(!core.contains("extension") && !core.value("nodes").toArray()[0].toObject().contains("position"));
    assert(core.value("nodes").toArray()[0].toObject().value("internal-data").toObject().value("extension")=="keep");
    assert(core.value("processing_version")==1 && desktop::read_graph_document(core).error.isEmpty());
    const auto restored=desktop::restore_graph_layout(core,document);
    assert(restored.value("extension")==42 && restored.value("nodes").toArray()[0].toObject().value("position")==document.value("nodes").toArray()[0].toObject().value("position"));
    auto invalid=document;
    invalid["nodes"]=QJsonArray{QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","???"}}}}};
    assert(!desktop::read_graph_document(invalid).error.isEmpty());
    const QJsonObject network{{"nodes",QJsonArray{QJsonObject{{"id",3},{"internal-data",QJsonObject{
        {"model-name","NetworkInput"},{"type","spectrum"},{"stream_id","s"}}}}}}, {"connections",QJsonArray{}}};
    const auto mapped=desktop::read_graph_document(network);
    assert(mapped.error.isEmpty());
    assert(std::get<domain::NetworkParameters>(mapped.graph.nodes.front().parameters).data_type==signal_contracts::FrameType::Spectrum);
    invalid=document;
    invalid["nodes"]=QJsonArray{QJsonObject{{"id",7},{"internal-data",QJsonObject{{"model-name","SpectrumGain"},{"gain","bad"}}}}};
    assert(!desktop::read_graph_document(invalid).error.isEmpty());
    for(const QJsonValue version:{QJsonValue(),QJsonValue("1"),QJsonValue(0),QJsonValue(-1),QJsonValue(1.25),QJsonValue(2)}) {
        invalid=document;invalid["processing_version"]=version;
        assert(!desktop::project_processing_graph(invalid));
        assert(desktop::processing_graph_document(invalid).isEmpty());
    }
    auto view=document;view["nodes"]=QJsonArray{QJsonObject{{"id",7},{"internal-data",QJsonObject{
        {"model-name","FFTView"},{"channel",2},{"merge",1},{"opaque",QJsonArray{1,2}}}}}};
    const auto projection=desktop::project_processing_graph(view);
    assert(projection);
    const auto parameters=projection.document["nodes"].toArray()[0].toObject()["internal-data"].toObject();
    assert(!parameters.contains("channel") && !parameters.contains("merge") && parameters["opaque"].isArray());
    assert(view["nodes"].toArray()[0].toObject()["internal-data"].toObject()["channel"]==2);
    const auto typed=desktop::read_processing_graph_document(view);
    assert(typed.error.isEmpty() && std::holds_alternative<domain::NoParameters>(typed.graph.nodes[0].parameters));
    assert(typed.document==view);
    const auto local=desktop::restore_graph_layout(projection.document,view);
    assert(local["nodes"].toArray()[0].toObject()["internal-data"].toObject()["channel"]==2);
}
