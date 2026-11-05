#include "editor_ui_test_fixture.h"
#include "isolated_gui_platform.h"
#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <control/numeric_desktop_host.h>
#include <pipeline/numeric_engine.h>
#include <QAction>
#include <QMenu>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}

struct ReferenceOutput {int node;unsigned short port;};
struct ReferencePreset {const char* id;std::span<const ReferenceOutput> outputs;};
constexpr std::array aubio_outputs{ReferenceOutput{33,4}};
constexpr std::array btrack_outputs{ReferenceOutput{34,1}};
constexpr std::array madmom_outputs{ReferenceOutput{4,0},ReferenceOutput{80,0},ReferenceOutput{81,0}};
constexpr std::array essentia_outputs{ReferenceOutput{50,1},ReferenceOutput{60,0}};
constexpr std::array presets{
    ReferencePreset{"aubio-reference",aubio_outputs},
    ReferencePreset{"reference-btrack",btrack_outputs},
    ReferencePreset{"madmom-superflux-reference",madmom_outputs},
    ReferencePreset{"essentia-reference",essentia_outputs}
};

bool connected(const domain::NumericProcessingGraph& graph,domain::NodeId from,domain::PortId port,
    domain::NodeId to) {
    return std::any_of(graph.edges.begin(),graph.edges.end(),[=](const auto& edge){
        return edge.from==from && edge.output_port==port && edge.to==to && edge.input_port==0;
    });
}

std::size_t node_count(const domain::NumericProcessingGraph& graph,std::string_view kind) {
    return std::count_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& node){return node.kind==kind;});
}

void verify_shared_frontend(const domain::NumericProcessingGraph& graph,std::string_view id) {
    require(node_count(graph,"AudioSource")==1,"Preset must have one shared source");
    if(id=="madmom-superflux-reference"){
        require(node_count(graph,"FeatureAggregate")==1 && node_count(graph,"NumberAccumulator")==1,
            "madmom must use a single SuperFlux history");
        for(const auto& output:madmom_outputs)
            require(connected(graph,31,0,output.node),"All madmom branches must read the same ODF snapshot");
    }
    if(id=="essentia-reference"){
        require(node_count(graph,"FrameBuffer")==1 && node_count(graph,"PeriodBeatTracker")==5,
            "Essentia must share the PCM session and first candidate");
        require(connected(graph,50,1,60) && connected(graph,50,1,103) && connected(graph,60,0,114),
            "Degara and MultiFeature must share the first candidate and expose both outputs");
    }
}

bool available_output(const pipeline::DeliveredNumericFrame& output) {
    if(output.status!=signal_contracts::Status::ok || !output.frame)return false;
    const auto frame=output.frame.view();
    const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);
    return frame.stamp.available && numeric && numeric->actual[0] && numeric->actual[1];
}

void record_output(const pipeline::DeliveredNumericFrame& frame,std::span<const pipeline::EndpointIdentity> endpoints,
    std::atomic_uint& seen) {
    if(!frame.context || !available_output(frame))return;
    const auto found=std::find(endpoints.begin(),endpoints.end(),frame.context->endpoint);
    if(found!=endpoints.end())seen.fetch_or(1u<<std::size_t(found-endpoints.begin()));
}

bool wait_for_outputs(pipeline::NumericEngine& engine,const std::string& graph,
    std::span<const ReferenceOutput> outputs) {
    std::vector<pipeline::EndpointIdentity> endpoints;
    for(const auto& output:outputs)
        endpoints.push_back({graph,output.node,pipeline::PortDirection::Output,output.port});
    auto seen=std::make_shared<std::atomic_uint>(0);
    const auto token=engine.subscribe_numeric_tap(endpoints,[seen,endpoints](const auto& frame){
        record_output(frame,endpoints,*seen);
    },{});
    require(token!=0,"Output subscription rejected");
    const auto complete=(1u<<outputs.size())-1;
    const bool published=editor_ui_test_fixture::wait_until([&]{return seen->load()==complete;},16000);
    engine.unsubscribe_numeric_tap(token);
    if(!published)std::fprintf(stderr,"Observed output mask %u, expected %u\n",seen->load(),complete);
    return published;
}

void verify_preset(CoreApplication& app,MainWindow& window,const ReferencePreset& preset) {
    std::fprintf(stderr,"GUI preset %s\n",preset.id);
    auto* action=window.findChild<QAction*>(QStringLiteral("example-")+QString::fromLatin1(preset.id));
    require(action && action->isEnabled(),"Research preset QAction missing");
    QSignalSpy applied(&app.presenter(),&GuiPresenter::graph_apply_finished);
    action->trigger();
    require(editor_ui_test_fixture::wait_until([&]{return !applied.isEmpty();},12000),"Research preset Apply did not finish");
    require(applied.last()[1].toBool(),"Research preset Apply rejected");
    auto& host=dynamic_cast<pipeline::NumericDesktopHost&>(app.presenter().client());
    domain::NumericProcessingGraph graph;
    const auto bytes=QJsonDocument(host.snapshot().graph).toJson(QJsonDocument::Compact);
    require(bool(desktop::decode_numeric_graph(bytes,graph)),"Applied graph could not be decoded");
    verify_shared_frontend(graph,preset.id);
    require(host.local_engine() && wait_for_outputs(*host.local_engine(),graph.graph_id,preset.outputs),
        "Applied reference preset did not publish every branch output");
    std::fprintf(stderr,"GUI preset %s published %zu outputs\n",preset.id,preset.outputs.size());
}

void run_presets(CoreApplication& app) {
    auto* window=editor_ui_test_fixture::main_window(require);
    const auto* menu=window->findChild<QMenu*>("rhythmReferencePresetsMenu");
    require(menu && menu->actions().size()==qsizetype(presets.size()),"Expected exactly four reference presets");
    require(editor_ui_test_fixture::wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 &&
        !app.presenter().graph_request_pending();},10000),"Startup graph unavailable");
    for(const auto& preset:presets)verify_preset(app,*window,preset);
}
}

int main() {
    if(!test_fixture::isolated_gui_platform())return 2;
    try {
        QTemporaryDir directory;
        require(directory.isValid(),"Cannot create isolated project directory");
        auto path=directory.filePath("project.json").toLocal8Bit();
        char name[]="rhythm_reference_gui_tests",option[]="--config";
        char* argv[]={name,option,path.data(),nullptr};
        int argc=3;
        CoreApplication app(argc,argv,test_fixture::synthetic_capture_factory(true));
        require(test_fixture::isolated_gui_platform(),"Expected offscreen Qt platform");
        run_presets(app);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what());
        return 1;
    }
}
