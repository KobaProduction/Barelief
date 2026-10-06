#include <domain/graph_validation.h>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace domain;
static void check(bool ok, const char* why) { if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
static NodeDefinition node(NodeId id, NodeKind kind) { return {id, kind, default_parameters(kind)}; }
int main() {
    auto caps = desktop_capabilities();
    check(caps.node_kinds.size() == 19, "canonical kind count");
    for (auto kind : caps.node_kinds) {
        check(node_kind(kind_id(kind)) == kind, "stable kind roundtrip");
        check(bool(validate_graph({"main", {node(0, kind)}, {}}, caps)) == (kind != NodeKind::NetworkInput && kind != NodeKind::NetworkOutput), "default parameters characterization");
    }
    check(!node_kind("SurfaceView"), "legacy alias must stay outside domain");
    GraphDefinition g{"main", {node(5, NodeKind::Mel), node(2, NodeKind::SpectrumGain)}, {{2, 5}}};
    auto valid = validate_graph(g, caps);
    check(valid && valid.topological_order == std::vector<NodeId>{2, 5}, "dependency order");
    g.nodes[1].enabled = false;
    check(bool(validate_graph(g, caps)), "gate must not alter port type");
    g.edges.clear(); check(bool(validate_graph(g, caps)), "disconnected draft can apply");
    g.edges = {{5,2}}; check(validate_graph(g,caps).error == GraphError::IncompatiblePorts, "Mel is not spectrum");
    g.edges = {{2,2}}; check(validate_graph(g,caps).error == GraphError::Cycle, "cycle");
    g.edges = {{2,5,1,0}}; check(validate_graph(g,caps).error == GraphError::MissingNodeOrPort, "invalid port");
    g.edges = {{2,5},{2,5}}; check(validate_graph(g,caps).error == GraphError::MultipleInputs, "single input");
    g.edges = {{99,5}}; check(validate_graph(g,caps).error == GraphError::MissingNodeOrPort, "missing node");
    g.edges.clear(); g.nodes[0].id=2; check(validate_graph(g,caps).error == GraphError::DuplicateNodeId, "duplicate id");
    g.nodes[0].id=-1; check(validate_graph(g,caps).error == GraphError::InvalidNodeId, "negative id");
    g.nodes[0].id=5;
    g.nodes[1].parameters=GainParameters{std::numeric_limits<double>::quiet_NaN()};
    check(validate_graph(g,caps).error == GraphError::InvalidParameters, "finite gain");
    g.nodes[1].parameters=MelParameters{}; check(validate_graph(g,caps).error == GraphError::InvalidParameters, "variant kind mismatch");
    g.nodes = {node(0,NodeKind::Window)}; g.nodes[0].parameters=WindowParameters{768};
    check(validate_graph(g,caps).error == GraphError::InvalidParameters, "window power of two");
    g.nodes={node(0,NodeKind::TestSignal)}; auto signal=TestSignalParameters{};
    signal.maximum_hz=23999; g.nodes[0].parameters=signal;
    check(validate_graph(g,caps).error == GraphError::InvalidParameters, "sweep below Nyquist");
    g.nodes={node(0,NodeKind::WLED),node(1,NodeKind::WLED)};
    check(validate_graph(g,caps).error == GraphError::OutputLimit, "singleton adapter capability");
    caps.max_wled_outputs=2; check(bool(validate_graph(g,caps)), "capability permits two outputs");
    caps.node_kinds.clear(); check(validate_graph(g,caps).error == GraphError::UnsupportedKind, "device kind capability");
    caps=desktop_capabilities(); g.nodes={node(0,NodeKind::NetworkInput)};
    g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"provider","mel","",8214};
    caps.receive_types={FrameType::PCM}; check(validate_graph(g,caps).error == GraphError::UnsupportedFrameType, "device receive capability");
    caps=desktop_capabilities(); check(bool(validate_graph(g,caps)), "typed network input");
    check(validate_graph(g,caps,{0,256}).error == GraphError::GraphLimit, "node budget");
    auto invalid_parameter=[&](NodeKind kind, NodeParameters parameters) {
        GraphDefinition candidate{"main",{{0,kind,std::move(parameters)}},{}};
        check(validate_graph(candidate,caps).error==GraphError::InvalidParameters,"invalid typed parameter accepted");
    };
    invalid_parameter(NodeKind::AudioSource,AudioSourceParameters{static_cast<AudioSource>(2)});
    invalid_parameter(NodeKind::SpectrumMerge,MergeParameters{static_cast<MergeMode>(-1)});
    invalid_parameter(NodeKind::FFTView,FFTViewParameters{static_cast<Channel>(3),MergeMode::RMS});
    invalid_parameter(NodeKind::WindowFunction,WindowFunctionParameters{static_cast<WindowFunction>(4)});
    invalid_parameter(NodeKind::SpectrumGain,GainParameters{100.01});
    invalid_parameter(NodeKind::WLED,GainParameters{-1});
    invalid_parameter(NodeKind::Mel,MelParameters{51});
    invalid_parameter(NodeKind::Spectrogram,HistoryParameters{1});
    invalid_parameter(NodeKind::Window,WindowParameters{8192});
    signal=TestSignalParameters{};signal.tones=4;invalid_parameter(NodeKind::TestSignal,signal);
    signal=TestSignalParameters{};signal.amplitude=std::numeric_limits<double>::infinity();invalid_parameter(NodeKind::TestSignal,signal);
    invalid_parameter(NodeKind::NetworkInput,NetworkParameters{FrameType::Mel,""," \t\n","",8214});
    invalid_parameter(NodeKind::NetworkOutput,NetworkParameters{FrameType::Mel,"","stream","",65536});
    g={"main",{{0,static_cast<NodeKind>(19),NoParameters{}}},{}};
    check(validate_graph(g,caps).error==GraphError::UnknownKind,"unknown kind");
    g={"main",{{std::numeric_limits<NodeId>::max(),NodeKind::Mel,MelParameters{}}},{}};
    check(validate_graph(g,caps).error==GraphError::InvalidNodeId,"legacy sentinel ID");
    g={"main",{node(0,NodeKind::NetworkOutput)},{}};
    g.nodes[0].parameters=NetworkParameters{FrameType::History,"","history","",8214};
    caps.publish_types={FrameType::Mel};
    check(validate_graph(g,caps).error==GraphError::UnsupportedFrameType,"publish capability");
    caps=desktop_capabilities();
    g={"main",{node(20,NodeKind::SpectrumGain),node(10,NodeKind::SpectrumGain),node(0,NodeKind::Mel)},{{20,10},{10,0}}};
    check(validate_graph(g,caps).topological_order==std::vector<NodeId>{20,10,0},"unsorted dependency chain");
    g.edges={{20,10},{10,20}};
    check(validate_graph(g,caps).error==GraphError::Cycle,"multi-node cycle");
    std::cout << "domain contracts passed\n";
}
