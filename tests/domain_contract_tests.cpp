#include <domain/graph_validation.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <array>
using namespace domain;
static void check(bool ok, const char* why) { if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
static NodeDefinition node(NodeId id, NodeKind kind) { return {id, kind, default_parameters(kind)}; }
int main() {
    {
        std::array<PortDescriptor,2> ins{}; std::array<PortDescriptor,8> outs{};
        ConfiguredNodeDescriptor d;
        auto history=node(42,NodeKind::Spectrogram);
        check(describe_node(history,ins,outs,d)==signal_contracts::Status::ok && d.outputs.size()==2,"configured history descriptor");
        check(d.outputs[0].identity==PortIdentity{42,0} && d.outputs[0].type==FrameType::History,"history identity");
        check(d.outputs[1].identity==PortIdentity{42,1} && d.outputs[1].quantity==signal_contracts::Quantity::FilledFrameCount && d.outputs[1].unit==signal_contracts::Unit::Frames,"count semantics");
        check(describe_node(history,ins,std::span(outs).first(1),d)==signal_contracts::Status::buffer_too_small && d.outputs.empty() && d.required_outputs==2,"descriptor buffer bound");
        auto bad=node(3,NodeKind::Mel); bad.parameters=GainParameters{};
        check(describe_node(bad,ins,outs,d)==signal_contracts::Status::invalid_argument,"wrong descriptor variant");
        check(!node_descriptor(static_cast<NodeKind>(-1)) && input_ports({3,static_cast<NodeKind>(-1),NoParameters{}})==0,"unknown kind descriptor");
        const auto* display=parameter_descriptor(NodeKind::FFTView,"merge");
        check(display && display->classification==ParameterClassification::Presentation,"FFT display classification");
        const auto* gain=parameter_descriptor(NodeKind::SpectrumGain,"gain");
        check(gain && gain->classification==ParameterClassification::Processing && gain->default_number==1 && gain->maximum==100,"gain descriptor bounds/default");
        check(parameter_classification(NodeKind::Mel,"unknown")==ParameterClassification::OpaqueExtension,"unknown parameter extension");
        auto gate=node(4,NodeKind::Gate);
        check(describe_node(gate,ins,outs,d)==signal_contracts::Status::ok && d.outputs.size()==1,"gate one output");
        auto selector=node(6,NodeKind::InputSelector);
        check(describe_node(selector,ins,outs,d)==signal_contracts::Status::ok && d.inputs.size()==2 && d.outputs.size()==1 && d.inputs[1].type==FrameType::PCM,"selector two PCM inputs/one output");
        check(describe_node(selector,std::span(ins).first(1),outs,d)==signal_contracts::Status::buffer_too_small && d.required_inputs==2,"selector descriptor input bound");
        auto draft_source=node(5,NodeKind::NetworkInput);
        check(output_ports(draft_source)==1 && output_type(draft_source)==FrameType::Mel,"invalid draft endpoint erased stable ports");
        auto legacy_view=node(7,NodeKind::FFTView);
        legacy_view.parameters=FFTViewParameters{Channel::Right,MergeMode::Maximum};
        check(validate_processing_graph({"view",{legacy_view},{}},desktop_capabilities()).error==GraphError::InvalidParameters,"typed view display variant accepted");
        auto processing_view=project_processing_node(legacy_view);
        check(std::holds_alternative<NoParameters>(processing_view.parameters) && std::get<FFTViewParameters>(legacy_view.parameters).channel==Channel::Right,"projection loses legacy display data");
        check(bool(validate_processing_graph({"view",{processing_view},{}},desktop_capabilities())) && input_type(processing_view)==FrameType::StereoSpectrum && output_ports(processing_view)==0,"typed NoParameters view");
        for(const auto& entry:node_descriptors()) {
            auto configured=node(13,entry.kind);
            if(auto* net=std::get_if<NetworkParameters>(&configured.parameters))net->stream_id="stream";
            check(describe_node(configured,ins,outs,d)==signal_contracts::Status::ok,"catalog descriptor incomplete");
            for(const auto& port:d.inputs)check(port.direction==PortDirection::Input && port.identity.node==13,"input identity/direction");
            for(const auto& port:d.outputs)check(port.direction==PortDirection::Output && port.identity.node==13,"output identity/direction");
        }
    }
    auto caps = desktop_capabilities();
    check(caps.node_kinds.size() == node_kind_count, "canonical kind count");
    check(node_role(NodeKind::TestSignal)==NodeRole::Source,"source role");
    check(node_role(NodeKind::FFTView)==NodeRole::Sink,"graph consumes only");
    check(node_role(NodeKind::Spectrogram)==NodeRole::Processor,"history accumulates, view does not");
    auto split=node(0,NodeKind::Splitter);
    split.parameters=RoutingParameters{FrameType::Mel,2,0};
    check(output_ports(split)==2 && output_type(split,1)==FrameType::Mel,"typed splitter outputs");
    GraphDefinition routes{"routes",{split,node(1,NodeKind::MelView)},{{0,1,1,0}}};
    check(bool(validate_graph(routes,caps)),"nonzero output port");
    routes.edges[0].output_port=2;
    check(validate_graph(routes,caps).error==GraphError::MissingNodeOrPort,"splitter port bound");
    routes.nodes[1]=node(1,NodeKind::SpectrogramView);routes.edges[0].output_port=0;
    check(validate_graph(routes,caps).error==GraphError::IncompatiblePorts,"shape and meaning both checked");
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
    {
        signal=TestSignalParameters{};signal.amplitude=2;
        auto error=validate_graph({"diagnostic",{{17,NodeKind::TestSignal,signal}},{}},caps);
        check(error.location.node==17 && error.location.parameter_key=="amplitude" && !error.location.edge_index && !error.location.direction && !error.location.port,"amplitude diagnostic location");
        signal=TestSignalParameters{};signal.minimum_hz=-1;
        error=validate_graph({"diagnostic",{{17,NodeKind::TestSignal,signal}},{}},caps);
        check(error.location.parameter_key=="minimum_hz","parameter diagnostics indistinguishable");
        auto mismatch=GraphDefinition{"diagnostic",{node(2,NodeKind::Mel),node(9,NodeKind::SpectrogramView)},{{2,9}}};
        error=validate_graph(mismatch,caps);
        check(error.location.node==9 && error.location.direction==PortDirection::Input && error.location.port==0 && error.location.edge_index==0,"connection diagnostic location");
        mismatch.edges[0].output_port=8;
        error=validate_graph(mismatch,caps);
        check(error.location.node==2 && error.location.direction==PortDirection::Output && error.location.port==8 && error.location.edge_index==0,"missing source port diagnostic");
        auto update=validate_processing_parameter_update(mismatch,77,NoParameters{});
        check(update.error==GraphError::MissingNodeOrPort && update.location.node==77 && update.location.parameter_key.empty() && !update.location.edge_index,"missing update identity");
        GraphDefinition views{"views",{{4,NodeKind::FFTView,NoParameters{}}},{}};
        update=validate_processing_parameter_update(views,4,FFTViewParameters{Channel::Right,MergeMode::Maximum});
        check(update.error==GraphError::InvalidParameters && update.location.node==4 && update.location.parameter_key=="channel" && !update.location.edge_index,"view update identity/classification");
        check(validate_processing_parameter_update(views,4,NoParameters{}).error==GraphError::InvalidParameters,"view empty update accepted");
        signal=TestSignalParameters{};signal.period_seconds=0;
        GraphDefinition analysis{"analysis",{node(5,NodeKind::TestSignal)},{}};
        update=validate_processing_parameter_update(analysis,5,signal);
        check(update.location.node==5 && update.location.parameter_key=="period_seconds","update canonical parameter key");
        check(bool(validate_processing_parameter_update(analysis,5,TestSignalParameters{})),"valid processing update rejected");
    }
    invalid_parameter(NodeKind::NetworkInput,NetworkParameters{FrameType::Mel,""," \t\n","",8214});
    invalid_parameter(NodeKind::NetworkOutput,NetworkParameters{FrameType::Mel,"","stream","",65536});
    g={"main",{{0,static_cast<NodeKind>(node_kind_count),NoParameters{}}},{}};
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
