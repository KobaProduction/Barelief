#include <core_app/numeric_default_graph.h>
#include <array>
#include <stdexcept>
#include <algorithm>
namespace {
domain::NumericProcessingGraph bind_example_configuration(domain::NumericProcessingGraph graph) {
    using namespace domain;
    graph.configuration_bindings.clear();
    const auto clock=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.kind=="SampleClock";});

    for(const auto& node:graph.nodes) {
        if(node.clock_node>=0)graph.configuration_bindings.push_back({node.clock_node,node.id});
        if(node.kind=="PcmResampler" && clock!=graph.nodes.end())graph.configuration_bindings.push_back({clock->id,node.id});
        if(node.kind=="AudioView" || node.kind=="FFTView" || node.kind=="MelView" || node.kind=="SpectrogramView") {
            const auto data_edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.to==node.id;});
            if(data_edge!=graph.edges.end())graph.configuration_bindings.push_back({data_edge->from,node.id});
            if(node.kind!="AudioView") {
                const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.to==node.id;});
                if(edge!=graph.edges.end()) {
                    auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==edge->from;});
                    if(source!=graph.nodes.end() && !configuration_output_supported(*source,ConfigurationRole::FrequencyGrid)) {
                        const auto upstream=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.to==source->id;});
                        if(upstream!=graph.edges.end())source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==upstream->from;});
                    }
                    if(source!=graph.nodes.end() && configuration_output_supported(*source,ConfigurationRole::FrequencyGrid))
                        graph.configuration_bindings.push_back({source->id,node.id,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid});
                }
            }
        }
    }
    NumericProcessingGraph resolved;
    const ProcessingAdmissionLimits limits{256,1024,64,64,64,16384,{8,32768,262144,16384,256,8192}};
    // Configuration associations are explicit; clock_node is derived executor cache.
    const auto result=resolve_configuration_bindings(graph,limits,resolved);
    if(!result)throw std::runtime_error("Example configuration binding admission failed: node "+std::to_string(result.node)+" error "+std::to_string(unsigned(result.error)));
    return resolved;
}
}
domain::NumericProcessingGraph numeric_default_graph() {
    using namespace domain;using namespace signal_contracts;
    constexpr FrameLimits limits{8,32768,262144,16384,256,8192};
    const std::array<std::uint32_t,1> channels{0};
    auto vector=[&](Semantic semantic,Axis axis,std::size_t count) {
        NumericContract c;c.dtype=DType::Float32;c.semantic=semantic;
        c.channel_order=channels;
        c.shape={Rank::Vector,{axis,Axis::None},{ExtentBounds{count,count},ExtentBounds{}}};
        c.clock={"local",{1,48000},48000};return c;
    };
    auto pcm=vector(Semantic::PCM,Axis::Sample,256);
    auto window=vector(Semantic::Window,Axis::Sample,4096);
    auto windowed=window;windowed.semantic=Semantic::Windowed;
    auto spectrum=vector(Semantic::SpectrumMagnitude,Axis::Bin,2048);
    spectrum.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{48000,4096}};
    auto mel=vector(Semantic::MelEnergy,Axis::Band,50);
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
    NumericProcessingGraph graph;graph.graph_id="main";graph.execution_interval={1,120};
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
    TransformProcessingParameters mel_parameters;
    mel_parameters.values.push_back({"bands",NumericLiteral{std::uint64_t(50)}});
    add(6,"Mel",&spectrum,&mel,mel_parameters);
    add(7,"Spectrogram",&mel,&history,HistoryProcessingParameters{400});
    graph.nodes.back().outputs.push_back(port(PortDirection::Output,1,count));
    add(8,"SpectrogramView",&history,nullptr);
    add(9,"AudioView",&pcm,nullptr);
    add(10,"FFTView",&spectrum,nullptr);
    add(11,"MelView",&mel,nullptr);
    add(12,"ScalarView",&count,nullptr);
    add(15,"BarkWeighting",&spectrum,&spectrum,TransformProcessingParameters{});
    graph.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0},{4,15,0,0},{15,6,0,0},{6,7,0,0},
        {7,8,0,0},{1,9,0,0},{15,10,0,0},{6,11,0,0},{7,12,1,0}};
    return bind_example_configuration(std::move(graph));
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
    return bind_example_configuration(std::move(graph));
}

domain::NumericProcessingGraph numeric_generator_graph() {
    auto graph=numeric_default_graph();
    auto& generator=graph.nodes.front();
    generator.kind="SignalGenerator";
    generator.parameters=domain::SignalGeneratorProcessingParameters{};
    generator.clock_node=16;
    domain::ConfiguredProcessingNode clock;
    clock.id=16;clock.kind="SampleClock";
    clock.parameters=domain::SampleClockProcessingParameters{};
    graph.nodes.push_back(std::move(clock));
    return bind_example_configuration(std::move(graph));
}

namespace {
std::vector<signal_contracts::OscillatorParameters> example_tones(unsigned channel) {
    std::vector<signal_contracts::OscillatorParameters> bank;
    for(unsigned i=0;i<8;++i)bank.push_back({i+1,true,(channel==1?73.25:110.3)+double(i*i)*(channel==1?233.7:197.2),.045,double(i)*.27,0,0});
    return bank;
}
domain::NumericProcessingGraph source_selector_graph(unsigned channels) {
    using namespace domain;using namespace signal_contracts;
    auto graph=numeric_generator_graph();
    std::get<SignalGeneratorProcessingParameters>(graph.nodes.front().parameters).oscillators=example_tones(1);
    const auto source=graph.nodes.front();
    if(channels==2) {
        auto generator=source;generator.id=101;
        std::get<SignalGeneratorProcessingParameters>(generator.parameters).oscillators=example_tones(2);
        graph.nodes.push_back(std::move(generator));
    }
    const auto pcm=source.outputs.front().contract;
    auto envelope=pcm.view();std::get<NumericContract>(envelope).shape.extents[0]={1,8192};
    OwnedProcessingContract output;
    if(own_processing_contract(envelope,FrameLimits{8,32768,262144,16384,256,8192},16384,output)!=Status::ok)throw std::runtime_error("Selector envelope admission failed");
    ConfiguredProcessingNode selector;selector.id=200;selector.kind="InputSelector";
    selector.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::int64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t(channels)}}}};
    for(unsigned lane=0;lane<channels;++lane)selector.outputs.push_back({PortId(lane),PortDirection::Output,output});
    for(unsigned stream=0;stream<3;++stream)for(unsigned lane=0;lane<channels;++lane)
        selector.inputs.push_back({PortId(stream*channels+lane),PortDirection::Input,pcm});
    for(auto& edge:graph.edges)if(edge.from==1||edge.from==101){edge.output_port=edge.from==1?0:1;edge.from=200;}
    for(auto& node:graph.nodes)if(node.id==2||node.id==9||node.id==102||node.id==109)node.inputs.front().contract=output;
    graph.edges.push_back({1,200,0,0});if(channels==2)graph.edges.push_back({101,200,0,1});
    for(unsigned stream=1;stream<3;++stream) {
        auto capture=numeric_default_graph().nodes.front();capture.id=stream==1?13:14;
        capture.parameters=EndpointProcessingParameters{{{"source",NumericLiteral{std::int64_t(stream-1)}}}};
        capture.outputs.clear();
        for(unsigned lane=0;lane<channels;++lane)capture.outputs.push_back({PortId(lane),PortDirection::Output,pcm});
        graph.nodes.push_back(std::move(capture));
        for(unsigned lane=0;lane<channels;++lane) {
            ConfiguredProcessingNode resampler;resampler.id=300+stream*32+lane;resampler.kind="PcmResampler";
            resampler.parameters=TransformProcessingParameters{{{"output_rate",NumericLiteral{std::uint64_t{48000}}}}};
            resampler.inputs={{0,PortDirection::Input,pcm}};
            auto resampled=pcm.view();std::get<NumericContract>(resampled).shape.extents[0]={1,257};
            OwnedProcessingContract resampled_owner;
            if(own_processing_contract(resampled,FrameLimits{8,32768,262144,16384,256,8192},16384,resampled_owner)!=Status::ok)throw std::runtime_error("Resampler admission failed");
            resampler.outputs={{0,PortDirection::Output,resampled_owner}};
            selector.inputs[stream*channels+lane].contract=resampled_owner;
            graph.edges.push_back({stream==1?13:14,resampler.id,PortId(lane),0});
            graph.edges.push_back({resampler.id,200,0,PortId(stream*channels+lane)});
            graph.nodes.push_back(std::move(resampler));
        }
    }
    graph.nodes.push_back(std::move(selector));
    if(channels==2) {
        // The common long transform consumes an explicit aligned mono mix;
        // each selected lane also has an independent short transform.
        ConfiguredProcessingNode mixer;mixer.id=210;mixer.kind="Mixer";
        MixerProcessingParameters mix;mix.mode=MixerMode::WeightedMean;
        mix.weights={{0,1.0},{1,1.0}};mix.consumption=MixerConsumption::SourceAligned;
        mixer.parameters=std::move(mix);
        mixer.inputs={{0,PortDirection::Input,output},{1,PortDirection::Input,output}};
        mixer.outputs={{0,PortDirection::Output,output}};
        graph.nodes.push_back(std::move(mixer));
        for(auto& edge:graph.edges)if(edge.from==200 && edge.to==2)edge.from=210;
        graph.edges.push_back({200,210,0,0});graph.edges.push_back({200,210,1,1});
        auto contract=[&](NodeId id) {
            return std::get<NumericContract>(std::find_if(graph.nodes.begin(),graph.nodes.end(),
                [=](const auto& n){return n.id==id;})->outputs.front().contract.view());
        };
        auto short_window=contract(2);short_window.shape.extents[0]={512,512};
        auto short_windowed=short_window;short_windowed.semantic=Semantic::Windowed;
        auto short_spectrum=contract(4);short_spectrum.shape.extents[0]={256,256};
        short_spectrum.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{48000,512}};
        auto own=[&](const NumericContract& value) {
            OwnedProcessingContract result;
            if(own_processing_contract(value,FrameLimits{8,32768,262144,16384,256,8192},16384,result)!=Status::ok)
                throw std::runtime_error("Stereo example contract admission failed");
            return result;
        };
        const auto window_owner=own(short_window),windowed_owner=own(short_windowed),spectrum_owner=own(short_spectrum);
        for(unsigned lane=0;lane<2;++lane) {
            const NodeId base=lane==0?100:400;
            ConfiguredProcessingNode buffer;buffer.id=base+2;buffer.kind="Window";
            buffer.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{512}}}}};
            buffer.inputs={{0,PortDirection::Input,output}};buffer.outputs={{0,PortDirection::Output,window_owner}};
            graph.nodes.push_back(std::move(buffer));
            ConfiguredProcessingNode window;window.id=base+3;window.kind="WindowFunction";
            window.parameters=TransformProcessingParameters{};
            window.inputs={{0,PortDirection::Input,window_owner}};window.outputs={{0,PortDirection::Output,windowed_owner}};
            graph.nodes.push_back(std::move(window));
            ConfiguredProcessingNode fft;fft.id=base+4;fft.kind="StereoFFT";
            fft.parameters=TransformProcessingParameters{};
            fft.inputs={{0,PortDirection::Input,windowed_owner}};fft.outputs={{0,PortDirection::Output,spectrum_owner}};
            graph.nodes.push_back(std::move(fft));
            ConfiguredProcessingNode view;view.id=base+10;view.kind="FFTView";
            view.inputs={{0,PortDirection::Input,spectrum_owner}};graph.nodes.push_back(std::move(view));
            graph.edges.insert(graph.edges.end(),{{200,base+2,PortId(lane),0},{base+2,base+3,0,0},
                {base+3,base+4,0,0},{base+4,base+10,0,0}});
        }
        auto scalar=contract(7);scalar.dtype=DType::Float32;scalar.semantic=Semantic::Scalar;
        scalar.quantity=Quantity::Unspecified;scalar.unit=NumericUnit::Unspecified;
        scalar.history_element=Semantic::Numeric;scalar.coordinates={};
        scalar.shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{1,1},ExtentBounds{1,1}}};
        const auto scalar_owner=own(scalar);
        ConfiguredProcessingNode comparison;comparison.id=220;comparison.kind="ChannelComparison";
        comparison.parameters=TransformProcessingParameters{{{"window_samples",NumericLiteral{std::uint64_t{512}}},
            {"silence_threshold",NumericLiteral{1e-12}}}};
        comparison.inputs={{0,PortDirection::Input,output},{1,PortDirection::Input,output}};
        comparison.outputs={{0,PortDirection::Output,scalar_owner}};
        graph.nodes.push_back(std::move(comparison));
        ConfiguredProcessingNode metric;metric.id=221;metric.kind="ScalarView";
        metric.inputs={{0,PortDirection::Input,scalar_owner}};graph.nodes.push_back(std::move(metric));
        graph.edges.insert(graph.edges.end(),{{200,220,0,0},{200,220,1,1},{220,221,0,0}});
    }
    return bind_example_configuration(std::move(graph));
}
}
domain::NumericProcessingGraph numeric_startup_graph() {return source_selector_graph(2);}

std::vector<NumericExampleGraph> numeric_example_graphs() {
    using namespace domain;
    // Each source is one explicit mono stream. Separate banks make both
    // branches visible; their shared SampleClock keeps sample positions aligned.
    const auto tones=example_tones;
    const auto generator_graph=[&] {
        auto graph=numeric_generator_graph();
        std::get<SignalGeneratorProcessingParameters>(graph.nodes.front().parameters).oscillators=tones(1);
        return bind_example_configuration(std::move(graph));
    };
    auto second_generator=[&](NumericProcessingGraph& graph) {
        auto generator=graph.nodes.front();generator.id=101;
        std::get<SignalGeneratorProcessingParameters>(generator.parameters).oscillators=tones(2);
        graph.nodes.push_back(std::move(generator));
    };
    auto mix=[](NumericProcessingGraph& graph,const OwnedProcessingContract& contract,NodeId first,NodeId second) {
        ConfiguredProcessingNode mixer;mixer.id=200;mixer.kind="Mixer";mixer.clock_node=16;
        mixer.parameters=MixerProcessingParameters{};
        mixer.inputs={{0,PortDirection::Input,contract},{1,PortDirection::Input,contract}};
        mixer.outputs={{0,PortDirection::Output,contract}};
        graph.nodes.push_back(std::move(mixer));
        graph.edges.push_back({first,200,0,0});graph.edges.push_back({second,200,0,1});
    };
    std::vector<NumericExampleGraph> examples;
    examples.push_back({"startup","Startup: generator, loopback, microphone",numeric_startup_graph()});
    examples.push_back({"stereo-sources","Stereo: generator, loopback, microphone",source_selector_graph(2)});
    auto pcm=generator_graph();second_generator(pcm);
    const auto pcm_contract=pcm.nodes.front().outputs.front().contract;
    mix(pcm,pcm_contract,1,101);
    for(auto& edge:pcm.edges)if(edge.from==1 && edge.to!=200)edge.from=200;
    examples.push_back({"pcm-mix","Two channels: PCM mixer",bind_example_configuration(std::move(pcm))});
    auto selector=generator_graph();second_generator(selector);
    ConfiguredProcessingNode select;select.id=200;select.kind="InputSelector";
    select.parameters=TransformProcessingParameters{{{"selected_input",signal_contracts::NumericLiteral{std::int64_t{0}}}}};
    select.inputs={{0,PortDirection::Input,pcm_contract},{1,PortDirection::Input,pcm_contract}};
    select.outputs={{0,PortDirection::Output,pcm_contract}};
    selector.nodes.push_back(std::move(select));
    for(auto& edge:selector.edges)if(edge.from==1)edge.from=200;
    selector.edges.push_back({1,200,0,0});selector.edges.push_back({101,200,0,1});
    examples.push_back({"input-selector","Two channels: input selector",bind_example_configuration(std::move(selector))});
    auto switched=generator_graph();
    ConfiguredProcessingNode output;output.id=200;output.kind="Switch";output.parameters=RouteProcessingParameters{2,0,true};
    output.inputs={{0,PortDirection::Input,pcm_contract}};
    output.outputs={{0,PortDirection::Output,pcm_contract},{1,PortDirection::Output,pcm_contract}};
    switched.nodes.push_back(std::move(output));
    for(auto& edge:switched.edges)if(edge.from==1){edge.from=200;if(edge.to==9)edge.output_port=1;}
    switched.edges.push_back({1,200,0,0});
    examples.push_back({"output-switch","Generator: output switch",bind_example_configuration(std::move(switched))});
    auto independent=generator_graph();auto branch=generator_graph();
    for(auto node:branch.nodes)if(node.id!=16){node.id+=100;if(node.clock_node>=0)node.clock_node=16;
        if(node.kind=="SignalGenerator")std::get<SignalGeneratorProcessingParameters>(node.parameters).oscillators=tones(2);
        independent.nodes.push_back(std::move(node));}
    for(auto edge:branch.edges){edge.from+=100;edge.to+=100;independent.edges.push_back(edge);}
    examples.push_back({"two-streams","Two channels: independent analysis",bind_example_configuration(std::move(independent))});
    auto spectral=generator_graph();second_generator(spectral);
    auto reference=generator_graph();
    for(auto node:reference.nodes)if(node.id==2 || node.id==3 || node.id==4 || node.id==15){node.id+=100;spectral.nodes.push_back(std::move(node));}
    spectral.edges.insert(spectral.edges.end(),{{101,102,0,0},{102,103,0,0},{103,104,0,0},{104,115,0,0}});
    const auto spectrum=std::find_if(spectral.nodes.begin(),spectral.nodes.end(),[](const auto& node){return node.id==15;})->outputs[0].contract;
    mix(spectral,spectrum,15,115);
    for(auto& edge:spectral.edges)if(edge.from==15 && edge.to!=200)edge.from=200;
    examples.push_back({"spectral-mix","Two channels: spectrum mixer",bind_example_configuration(std::move(spectral))});
    return examples;
}
