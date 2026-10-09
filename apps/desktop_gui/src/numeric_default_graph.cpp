#include <core_app/numeric_default_graph.h>
#include <array>
#include <stdexcept>
domain::NumericProcessingGraph numeric_default_graph() {
    using namespace domain;using namespace signal_contracts;
    constexpr FrameLimits limits{8,32768,262144,16384,256,8192};
    const std::array<std::uint32_t,2> channels{0,1};
    auto vector=[&](Semantic semantic,Axis axis,std::size_t count,bool stereo) {
        NumericContract c;c.dtype=DType::Float32;c.semantic=semantic;
        c.channel_order=std::span(channels).first(stereo?2:1);
        c.shape={Rank::Vector,{axis,Axis::None},{ExtentBounds{count,count},ExtentBounds{}}};
        c.clock={"local",{1,48000},48000};return c;
    };
    auto pcm=vector(Semantic::PCM,Axis::Sample,256,true);
    auto window=vector(Semantic::Window,Axis::Sample,4096,true);
    auto windowed=window;windowed.semantic=Semantic::Windowed;
    auto spectrum=vector(Semantic::SpectrumMagnitude,Axis::Bin,2048,true);
    spectrum.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{48000,4096}};
    auto mono=spectrum;mono.channel_order=std::span(channels).first(1);
    auto mel=vector(Semantic::MelEnergy,Axis::Band,50,false);
    // The processing runtime owns the actual nonlinear filter-bank centres.
    mel.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
    auto history=mel;history.semantic=Semantic::History;history.history_element=Semantic::MelEnergy;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,400},ExtentBounds{50,50}}};
    // Per-row source-clock times survive rolling snapshots and remote resync.
    history.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
    history.coordinates[1]=mel.coordinates[0];
    NumericContract count;count.dtype=DType::UInt64;count.semantic=Semantic::Scalar;
    count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
    count.channel_order=std::span(channels).first(1);count.clock=mel.clock;
    auto port=[&](PortDirection direction,PortId id,const NumericContract& contract) {
        ProcessingPort p;p.direction=direction;p.id=id;
        if(own_processing_contract(contract,limits,16384,p.contract)!=Status::ok)
            throw std::runtime_error("Default numeric contract admission failed");
        return p;
    };
    NumericProcessingGraph graph;graph.graph_id="main";
    auto add=[&](NodeId id,const char* kind,const NumericContract* input,const NumericContract* output,
        ProcessingParameters parameters=EndpointProcessingParameters{}) {
        ConfiguredProcessingNode node;node.id=id;node.kind=kind;node.parameters=std::move(parameters);
        if(input)node.inputs.push_back(port(PortDirection::Input,0,*input));
        if(output)node.outputs.push_back(port(PortDirection::Output,0,*output));
        graph.nodes.push_back(std::move(node));
    };
    TransformProcessingParameters window_parameters;
    window_parameters.values.push_back({"size",NumericLiteral{std::uint64_t(4096)}});
    add(1,"AudioSource",nullptr,&pcm);
    add(2,"Window",&pcm,&window,window_parameters);
    add(3,"WindowFunction",&window,&windowed,TransformProcessingParameters{});
    add(4,"StereoFFT",&windowed,&spectrum,TransformProcessingParameters{});
    add(5,"SpectrumMerge",&spectrum,&mono,TransformProcessingParameters{});
    TransformProcessingParameters mel_parameters;
    mel_parameters.values.push_back({"bands",NumericLiteral{std::uint64_t(50)}});
    add(6,"Mel",&mono,&mel,mel_parameters);
    add(7,"Spectrogram",&mel,&history,HistoryProcessingParameters{400});
    graph.nodes.back().outputs.push_back(port(PortDirection::Output,1,count));
    add(8,"SpectrogramView",&history,nullptr);
    add(9,"AudioView",&pcm,nullptr);
    add(10,"FFTView",&mono,nullptr);
    add(11,"MelView",&mel,nullptr);
    add(12,"ScalarView",&count,nullptr);
    add(15,"BarkWeighting",&spectrum,&spectrum,TransformProcessingParameters{});
    graph.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0},{4,15,0,0},{15,5,0,0},{5,6,0,0},{6,7,0,0},
        {7,8,0,0},{1,9,0,0},{5,10,0,0},{6,11,0,0},{7,12,1,0}};
    return graph;
}

domain::NumericProcessingGraph numeric_test_graph() {
    auto graph=numeric_default_graph();
    auto& source=graph.nodes.front();source.kind="TestSignal";
    source.parameters=domain::EndpointProcessingParameters{{
        {"tones",signal_contracts::NumericLiteral{std::uint64_t{8}}},
        {"sample_rate",signal_contracts::NumericLiteral{std::uint64_t{48000}}},
        {"minimum_hz",signal_contracts::NumericLiteral{73.0}},
        {"maximum_hz",signal_contracts::NumericLiteral{13901.0}},
        {"amplitude",signal_contracts::NumericLiteral{0.4}},
        {"period_seconds",signal_contracts::NumericLiteral{6.0}},
        {"sweep_depth",signal_contracts::NumericLiteral{0.18}}
    }};
    return graph;
}

domain::NumericProcessingGraph numeric_startup_graph() {
    auto graph=numeric_test_graph();
    auto bark=std::move(graph.nodes.back());graph.nodes.pop_back();
    auto capture=numeric_default_graph().nodes.front();capture.id=13;
    domain::ConfiguredProcessingNode selector;
    selector.id=14;selector.kind="InputSelector";
    selector.parameters=domain::TransformProcessingParameters{{
        {"selected_input",signal_contracts::NumericLiteral{std::uint64_t{0}}}
    }};
    auto input=graph.nodes.front().outputs.front();
    input.direction=domain::PortDirection::Input;
    selector.inputs.push_back(input);input.id=1;selector.inputs.push_back(input);
    selector.outputs=graph.nodes.front().outputs;
    graph.nodes.push_back(std::move(capture));graph.nodes.push_back(std::move(selector));graph.nodes.push_back(std::move(bark));
    for(auto& edge:graph.edges)if(edge.from==1)edge.from=14;
    graph.edges.push_back({1,14,0,0});graph.edges.push_back({13,14,0,1});
    return graph;
}
