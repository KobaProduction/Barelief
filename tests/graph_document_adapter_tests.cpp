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
}
