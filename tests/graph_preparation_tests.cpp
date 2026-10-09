// Assertions include setup calls and must execute in Release as well.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <pipeline/graph.h>
#include <pipeline/audio_pipeline.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <cassert>
using namespace pipeline;
static QJsonObject node(int id, const char *kind, QJsonObject parameters = {}) {
    parameters["model-name"] = kind;
    return {{"id", id}, {"internal-data", parameters}};
}
static QJsonObject edge(int from, int to) {
    return {{"outNodeId", from}, {"inNodeId", to}, {"outPortIndex", 0}, {"inPortIndex", 0}};
}
class StopTickSource final : public IAudioSource {
public:
    explicit StopTickSource(std::shared_ptr<std::function<void()>> tick) : tick(std::move(tick)) {}
    void start() override { running=true; }
    void stop() override { if(running) { running=false; if(*tick) (*tick)(); } }
    bool is_running() const override { return running; }
    AudioSourceKind source() const override { return AudioSourceKind::TestSignal; }
    void set_audio_handler(AudioHandler) override {}
    void set_status_handler(StatusHandler) override {}
    void set_lifecycle_handler(LifecycleHandler) override {}
private:
    std::shared_ptr<std::function<void()>> tick;
    bool running=false;
};
static void external_history_defaults() {
    AudioPipeline desktop([](AudioSourceKind){return std::unique_ptr<IAudioSource>{};});QString error;
    QJsonObject graph{{"version",2},{"nodes",QJsonArray{
        node(0,"NetworkInput",{{"data_type","history"},{"stream_id","history"}}),
        node(1,"Gate",{{"data_type","history"}}),node(2,"SpectrogramView"),node(10,"TestSignal",{{"amplitude",.1}}),
        node(20,"NetworkInput",{{"data_type","history"},{"stream_id","second"}}),node(21,"SpectrogramView")}},
        {"connections",QJsonArray{edge(0,1),edge(1,2),edge(20,21)}}};
    assert(desktop.apply_graph(graph,error));desktop.set_view_subscription(0);
    TypedFrame input;input.type=FrameType::History;input.sample_rate=48000;input.epoch=700;input.graph_revision=800;input.history={{1,2},{3,4}};
    const EndpointIdentity sink{"main",2,PortDirection::Input,0};
    desktop.feed_typed(0,input);
    assert(desktop.latest_views()[2].history==input.history);
    auto canonical=desktop.latest_port_frames({sink});assert(canonical.size()==1 && canonical[0].frame.history==input.history);
    assert(input.sequence==0 && input.producer_epoch==0 && input.history_capacity==0);
    assert(canonical[0].frame.sequence==1 && canonical[0].frame.producer_epoch==700 && canonical[0].frame.history_capacity==2 &&
           canonical[0].frame.epoch==700 && canonical[0].frame.graph_revision==800);
    TypedFrame second;second.type=FrameType::History;second.sample_rate=48000;second.epoch=1700;second.producer_epoch=1701;
    second.graph_revision=1800;second.sequence=77;second.generation=9;second.history_capacity=4;second.source_sequence=100;second.timestamp_ns=123;
    second.history={{11,12},{13,14}};desktop.feed_typed(20,second);
    const EndpointIdentity second_sink{"main",21,PortDirection::Input,0};
    const auto second_owned=desktop.latest_port_frames({second_sink})[0].frame;
    assert(second_owned.history==second.history && second_owned.sequence==77 && second_owned.epoch==1700 && second_owned.producer_epoch==1701 &&
           second_owned.graph_revision==1800 && second_owned.history_capacity==4 && second_owned.source_sequence==100 && second_owned.timestamp_ns==123);
    assert(second.sequence==77 && second.epoch==1700 && second.producer_epoch==1701 && second.graph_revision==1800 && second.history_capacity==4);
    second.history.clear();assert(desktop.latest_views()[21].history==second_owned.history);
    auto nodes=graph["nodes"].toArray();nodes[3]=node(10,"TestSignal",{{"amplitude",.2}});graph["nodes"]=nodes;
    assert(desktop.apply_graph(graph,error));assert(desktop.latest_views()[2].history==input.history);
    canonical=desktop.latest_port_frames({sink});assert(canonical[0].frame.history==input.history && canonical[0].frame.sequence==1);
    assert(desktop.latest_port_frames({second_sink})[0].frame.history==second_owned.history && desktop.latest_views()[21].history==second_owned.history);
    nodes=graph["nodes"].toArray();nodes[1]=node(1,"Gate",{{"data_type","history"},{"enabled",false}});graph["nodes"]=nodes;
    assert(desktop.apply_graph(graph,error));input.history={{3,4},{5,6}};desktop.feed_typed(0,input);input.history={{5,6},{7,8}};desktop.feed_typed(0,input);
    const std::vector<std::vector<float>> silence{{0,0},{0,0}};
    assert(desktop.latest_views()[2].history==silence);canonical=desktop.latest_port_frames({sink});assert(canonical[0].frame.history==silence);
    const auto independent=desktop.latest_port_frames({second_sink})[0].frame;
    assert(independent.history==second_owned.history && independent.sequence==77 && independent.producer_epoch==1701 && independent.graph_revision==1800);
    assert(input.history==std::vector<std::vector<float>>({{5,6},{7,8}}) && input.sequence==0 && input.producer_epoch==0 && input.history_capacity==0);
    input.history.clear();nodes=graph["nodes"].toArray();nodes[3]=node(10,"TestSignal",{{"amplitude",.3}});graph["nodes"]=nodes;
    assert(desktop.apply_graph(graph,error));assert(desktop.latest_views()[2].history==silence);
    assert(desktop.latest_port_frames({sink})[0].frame.history==silence);
    assert(desktop.latest_port_frames({second_sink})[0].frame.history==second_owned.history);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if(app.arguments().contains("--external-history-only")){external_history_defaults();return 0;}
    QString error;
    auto doc = GraphProgram::defaults();
    auto nodes = doc["nodes"].toArray();
    nodes.append(node(100, "AudioSource"));
    nodes.append(node(101, "AudioView"));
    doc["nodes"] = nodes;
    auto edges = doc["connections"].toArray();
    edges.append(edge(100, 101));
    doc["connections"] = edges;
    auto p = GraphProgram::compile(doc, error);
    assert(p);
    GraphEvaluationContext context;
    TypedFrame f;
    f.type = FrameType::PCM;
    f.sample_rate = 48000;
    f.sequence = 1;
    f.left.resize(8192);
    f.right = f.left;
    auto out = p->evaluate(0, f, context);
    assert(out.windows_processed == 10 && p->history_snapshot(7, context).size() == 10);
    auto small = f;
    small.left.resize(4096);
    small.right.resize(4096);
    assert(p->evaluate(100, small, context).views.contains(101));
    for (int i = 0; i < nodes.size(); ++i) {
        auto n = nodes[i].toObject();
        if (n["id"].toInt() != 8)
            continue;
        auto params = n["internal-data"].toObject();
        params["gain"] = 2;
        n["internal-data"] = params;
        nodes[i] = n;
    }
    doc["nodes"] = nodes;
    auto q = GraphProgram::compile(doc, error);
    assert(q);
    auto resets = q->rebind(context, *p);
    assert(resets.empty() && q->history_snapshot(7, context).size() == 10);
    f.sequence = 2;
    assert(q->evaluate(0, f, context).windows_processed == 10 && q->history_snapshot(7, context).size() == 20);
    // A failed resource negotiation preserves the working context and reports
    // the failure explicitly to the Apply owner.
    QJsonArray history_nodes{node(0, "NetworkInput", {{"data_type", "mel"}, {"stream_id", "mel"}})};
    QJsonArray history_edges;
    for (int id = 1; id <= 3; ++id) {
        history_nodes.append(node(id, "Spectrogram", {{"history_size", 2}}));
        history_edges.append(edge(0, id));
    }
    QJsonObject small_doc{{"version", 2}, {"nodes", history_nodes}, {"connections", history_edges}};
    const auto small_document=small_doc;
    p = GraphProgram::compile(small_doc, error);
    assert(p);
    context = {};
    TypedFrame mel;
    mel.type = FrameType::Mel;
    mel.sample_rate = 48000;
    mel.sequence = 1;
    mel.values.resize(4096, 1);
    p->evaluate(0, mel, context);
    assert(p->history_snapshot(1, context).size() == 1);
    for (int i = 1; i < history_nodes.size(); ++i) {
        auto n = history_nodes[i].toObject();
        auto params = n["internal-data"].toObject();
        params["history_size"] = 2000;
        n["internal-data"] = params;
        history_nodes[i] = n;
    }
    small_doc["nodes"] = history_nodes;
    q = GraphProgram::compile(small_doc, error);
    assert(q);
    std::vector<int> failed_resets;
    signal_contracts::Status status;
    failed_resets = q->rebind(context, *p, 0, 0, &status);
    assert(status == signal_contracts::Status::exhausted && failed_resets.empty() &&
           p->history_snapshot(1, context).size() == 1);
    mel.sequence = 2;
    p->evaluate(0, mel, context);
    assert(p->history_snapshot(1, context).size() == 2);
    // Exercise the actual application lifecycle, not only the JSON compatibility
    // adapter: resource failure must retain document, epoch, revision and state.
    AudioPipeline bounded;
    auto desktop_small=small_document;
    auto desktop_nodes=desktop_small["nodes"].toArray();desktop_nodes.append(node(4,"SpectrogramView"));
    auto desktop_edges=desktop_small["connections"].toArray();desktop_edges.append(edge(1,4));
    desktop_small["nodes"]=desktop_nodes;desktop_small["connections"]=desktop_edges;
    assert(bounded.apply_graph(desktop_small,error));
    mel.sequence=1;bounded.feed_typed(0,mel);
    assert(bounded.latest_views()[4].history.size()==1);
    const auto working=bounded.graph();const auto epoch=bounded.epoch(),revision=bounded.graph_revision();
    auto desktop_large=small_doc;
    desktop_nodes=desktop_large["nodes"].toArray();desktop_nodes.append(node(4,"SpectrogramView"));
    desktop_large["nodes"]=desktop_nodes;desktop_large["connections"]=desktop_edges;
    assert(!bounded.apply_graph(desktop_large,error) && error.contains("Graph preparation failed") &&
           bounded.graph()==working && bounded.epoch()==epoch && bounded.graph_revision()==revision &&
           bounded.latest_views()[4].history.size()==1);
    mel.sequence=2;bounded.feed_typed(0,mel,epoch);
    assert(bounded.latest_views()[4].history.size()==2);
    bounded.stop_analyzer();
    // Reusing an ingress ID for a local producer must discard the negotiated
    // remote History contract, while still accepting the valid graph on Apply.
    AudioPipeline desktop;
    desktop.stop_capture();
    desktop.stop_analyzer();
    // Registered legacy ingress counters survive physical producer reconfiguration
    // together with the compatible remote ring (no native source is opened).
    AudioPipeline legacy([](AudioSourceKind){return std::unique_ptr<IAudioSource>{};});
    auto mixed=QJsonObject{{"version",2},{"nodes",QJsonArray{
        node(0,"NetworkInput",{{"data_type","mel"},{"stream_id","mel"}}),node(1,"Spectrogram",{{"history_size",4}}),
        node(2,"SpectrogramView"),node(10,"TestSignal",{{"amplitude",0.1}})}},
        {"connections",QJsonArray{edge(0,1),edge(1,2)}}};
    assert(legacy.apply_graph(mixed,error));
    TypedFrame old;old.type=FrameType::Mel;old.sample_rate=48000;old.values={1,2,3};
    legacy.feed_typed(0,old);legacy.feed_typed(0,old);
    assert(legacy.latest_views()[2].history.size()==2);
    auto mixed_nodes=mixed["nodes"].toArray();mixed_nodes[3]=node(10,"TestSignal",{{"amplitude",0.2}});mixed["nodes"]=mixed_nodes;
    assert(legacy.apply_graph(mixed,error));legacy.feed_typed(0,old);
    assert(legacy.latest_views()[2].history.size()==3);
    legacy.stop_capture();legacy.stop_analyzer();
    // A network tick accepted while a physical source joins must survive Apply.
    // The fake stop hook fixes the interleaving without timing or native capture.
    auto stop_tick=std::make_shared<std::function<void()>>();
    AudioPipeline concurrent([stop_tick](AudioSourceKind){return std::make_unique<StopTickSource>(stop_tick);});
    assert(concurrent.apply_graph(mixed,error));
    old.sequence=1;concurrent.feed_typed(0,old);
    old.sequence=2;concurrent.feed_typed(0,old);
    *stop_tick=[&] {
        old.sequence=3;old.values={7,8,9};concurrent.feed_typed(0,old);
        assert(concurrent.latest_views()[2].history.size()==3);
    };
    mixed_nodes=mixed["nodes"].toArray();mixed_nodes[3]=node(10,"TestSignal",{{"amplitude",0.3}});mixed["nodes"]=mixed_nodes;
    assert(concurrent.apply_graph(mixed,error));
    *stop_tick={};
    assert(concurrent.latest_views()[2].history.size()==3);
    assert(concurrent.latest_views()[2].history.back()==old.values);
    old.sequence=4;concurrent.feed_typed(0,old);
    assert(concurrent.latest_views()[2].history.size()==4);
    concurrent.stop_capture();concurrent.stop_analyzer();
    QJsonObject remote{{"version",2},{"nodes",QJsonArray{
        node(0,"NetworkInput",{{"data_type","history"},{"stream_id","h"}}),node(1,"SpectrogramView")}},
        {"connections",QJsonArray{edge(0,1)}}};
    assert(desktop.apply_graph(remote,error));
    desktop.start_analyzer();
    TypedFrame history;
    history.type=FrameType::History;history.sample_rate=48000;history.sequence=1;history.history={{1,2},{3,4}};
    desktop.feed_typed(0,history);
    assert(desktop.latest_views()[1].history==history.history);
    QJsonObject replacement{{"version",2},{"nodes",QJsonArray{
        node(0,"TestSignal"),node(1,"AudioView")}}, {"connections",QJsonArray{edge(0,1)}}};
    assert(desktop.apply_graph(replacement,error));
    desktop.stop_capture();
    desktop.stop_analyzer();
}
