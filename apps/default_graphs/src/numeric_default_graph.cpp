#include <core_app/numeric_default_graph.h>
#include "example_builder.h"
#include "startup_frequency_profile.h"
void append_startup_rhythm(examples::Builder&);
#include <algorithm>
#include <array>
#include <stdexcept>
namespace {
using namespace domain;using namespace signal_contracts;
TransformProcessingParameters regular_spectrum_grid(std::uint32_t sample_rate,std::uint64_t fft_size) {
    return examples::transform({{"sample_rate",NumericLiteral{std::uint64_t{sample_rate}}},
        {"bin_hz",NumericLiteral{double(sample_rate)/double(fft_size)}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"frequencies_hz",NumericArray{}}});
}
std::vector<OscillatorParameters> example_tones(unsigned channel) {
    std::vector<OscillatorParameters> bank;
    for(unsigned i=0;i<8;++i)bank.push_back({i+1,true,(channel==1?73.25:110.3)+double(i*i)*(channel==1?233.7:197.2),.045,double(i)*.27,0,0});
    return bank;
}
NumericProcessingGraph finish_example(NumericProcessingGraph graph) {
    examples::Builder builder;builder.graph=std::move(graph);return builder.finish();
}
void set_generator_tones(examples::Builder& b,NodeId id,unsigned channel) {
    std::get<SignalGeneratorProcessingParameters>(b.node(id).parameters).oscillators=example_tones(channel);
}
void add_second_generator(examples::Builder& b) {
    b.add(101,"SignalGenerator");set_generator_tones(b,101,2);
    b.node(101).clock_node=16;b.configuration(16,101);
}
void redirect_source_edge(examples::Builder& builder,Edge& edge,NodeId to,PortId rate_port) {
    const auto port=edge.output_port==sample_rate_output?rate_port:edge.output_port;
    const auto& outputs=builder.node(to).outputs;
    if(std::none_of(outputs.begin(),outputs.end(),[=](const auto& p){return p.id==port;}))return;
    edge.from=to;edge.output_port=port;
    const auto contract=builder.output(to,port);
    for(auto& input:builder.node(edge.to).inputs)if(input.id==edge.input_port)input.contract=contract;
}
void replace_source(examples::Builder& b,NodeId from,NodeId to,PortId rate_port=sample_rate_output) {
    for(auto& edge:b.graph.edges)if(edge.from==from && edge.to!=to)redirect_source_edge(b,edge,to,rate_port);
}
void add_mixer(examples::Builder& b,NodeId id,NodeId first,NodeId second,PortId second_port=0,bool aligned=false) {
    MixerProcessingParameters parameters;parameters.consumption=aligned?MixerConsumption::SourceAligned:MixerConsumption::ClockedFifo;
    b.chain(id,"Mixer",first,parameters);b.connect(second,id,second_port,1);b.derive(id);
    if(!aligned){b.node(id).clock_node=16;b.configuration(16,id);}
}
OwnedProcessingContract selector_packet_contract(examples::Builder& b) {
    auto data=b.output(1).view();
    auto& value=std::get<NumericContract>(data);
    // Explicit bounded desktop delivery envelope, independent of selected Fs.
    value.shape.extents[0]={1,8192};
    OwnedProcessingContract result;
    if(own_processing_contract(value,b.limits.frame,b.limits.max_owned_metadata_bytes,result)!=Status::ok)
        throw std::runtime_error("Selector packet envelope exceeds example resources");
    return result;
}
void append_selector_inputs(ConfiguredProcessingNode& selector,unsigned lanes,unsigned streams,
                            const OwnedProcessingContract& pcm,const OwnedProcessingContract& rate) {
    for(unsigned stream=0;stream<streams;++stream)for(unsigned lane=0;lane<=lanes;++lane)
        selector.inputs.push_back({PortId(stream*(lanes+1)+lane),PortDirection::Input,lane==lanes?rate:pcm});
}
void add_selector(examples::Builder& b,unsigned lanes,unsigned streams) {
    const auto pcm=selector_packet_contract(b),rate=b.output(1,sample_rate_output);
    b.add(200,"InputSelector",examples::transform({{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t(lanes+1)}}}));
    auto& selector=b.node(200);selector.inputs.clear();selector.outputs.clear();
    for(unsigned lane=0;lane<lanes;++lane)selector.outputs.push_back({PortId(lane),PortDirection::Output,pcm});
    selector.outputs.push_back({PortId(lanes),PortDirection::Output,rate});
    replace_source(b,1,200,PortId(lanes));
    append_selector_inputs(selector,lanes,streams,pcm,rate);
}
void connect_generator_stream(examples::Builder& b,unsigned lanes) {
    b.connect(1,200);
    if(lanes==2)b.connect(101,200,0,1);
    b.connect(1,200,sample_rate_output,PortId(lanes));
}
void add_capture_source(examples::Builder& b,NodeId id,unsigned source,unsigned lanes) {
    b.add(id,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::int64_t(source)}},{"output_count",NumericLiteral{std::uint64_t(lanes)}}}});
    const auto pcm=b.output(id);
    for(unsigned lane=1;lane<lanes;++lane){auto& outputs=b.node(id).outputs;
        const auto metadata=std::find_if(outputs.begin(),outputs.end(),[](const auto& p){return p.id==sample_rate_output;});
        outputs.insert(metadata,{PortId(lane),PortDirection::Output,pcm});}
}
void connect_capture_lane(examples::Builder& b,NodeId capture,unsigned stream,unsigned lane,unsigned lanes) {
    const auto id=NodeId(300+stream*32+lane);
    b.add(id,"PcmResampler",examples::transform({{"output_rate",NumericLiteral{std::uint64_t{48000}}}}));
    b.connect(capture,id,PortId(lane));b.connect(capture,id,sample_rate_output,input_sample_rate_input);b.derive(id);
    b.connect(id,200,0,PortId(stream*(lanes+1)+lane));
    if(lane==0)b.connect(id,200,sample_rate_output,PortId(stream*(lanes+1)+lanes));
}
void add_capture_stream(examples::Builder& b,unsigned stream,unsigned lanes) {
    const auto id=NodeId(stream==1?13:14);add_capture_source(b,id,stream-1,lanes);
    for(unsigned lane=0;lane<lanes;++lane)connect_capture_lane(b,id,stream,lane,lanes);
}
void add_short_spectrum(examples::Builder& b,unsigned lane) {
    const NodeId base=lane==0?100:400;
    examples::add_window(b,200,base+2,base+3,base+20,base+21,512,PortId(lane));
    b.chain(base+4,"StereoFFT",base+21);b.chain(base+10,"AudioLineView",base+4);
    b.add(base+22,"FrequencyStep");b.connect(200,base+22,2,0);
    b.connect(base+20,base+22,0,window_size_input);
    b.connect(base+22,base+10,0,frequency_step_input);
}
void add_stereo_analysis(examples::Builder& b) {
    add_mixer(b,210,200,200,1,true);
    for(auto& edge:b.graph.edges)if(edge.from==200 && edge.to==2){edge.from=210;edge.output_port=0;}
    add_short_spectrum(b,0);add_short_spectrum(b,1);
    b.chain(220,"ChannelComparison",200,examples::transform({{"window_samples",NumericLiteral{std::uint64_t{512}}},{"silence_threshold",NumericLiteral{1e-12}}}));
    b.connect(200,220,1,1);b.derive(220);b.chain(221,"ScalarMetricsView",220);
}
NumericProcessingGraph source_selector_graph(unsigned lanes) {
    examples::Builder b;b.graph=numeric_generator_graph();set_generator_tones(b,1,1);
    if(lanes==2)add_second_generator(b);
    add_selector(b,lanes,3);connect_generator_stream(b,lanes);
    add_capture_stream(b,1,lanes);add_capture_stream(b,2,lanes);b.derive(200);
    if(lanes==2)add_stereo_analysis(b);
    return b.finish();
}
void add_temporal_source(examples::Builder& b) {
    auto& tones=std::get<SignalGeneratorProcessingParameters>(b.node(1).parameters).oscillators;
    tones.clear();constexpr double frequencies[]{73,74.5,220,222,880,882.25,3520,3523};
    for(unsigned i=0;i<std::size(frequencies);++i)tones.push_back({i+1,true,frequencies[i],.05,double(i)*.27,0,0});
    add_capture_source(b,13,0,1);add_selector(b,1,2);connect_generator_stream(b,1);
    b.connect(13,200,0,2);b.connect(13,200,sample_rate_output,3);b.derive(200);
}
void add_temporal_transform(examples::Builder& b) {
    b.chain(21,"MatrixTranspose",7);
    b.chain(23,"MatrixCenter",21,examples::transform({{"method",NumericLiteral{std::int64_t{0}}},{"offset",NumericLiteral{0.0}}}));
    b.chain(22,"MatrixWindowFunction",23,examples::transform({{"function",std::string{"hann"}}}));b.node(22).enabled=false;
    b.chain(17,"MatrixFFT",22,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{512}}}}));
    b.chain(18,"AudioSurfaceView",17);b.chain(19,"MatrixRow",17,examples::transform({{"row",NumericLiteral{std::uint64_t{0}}}}));
    b.chain(20,"AudioLineView",19,examples::transform({{"first_bin",NumericLiteral{std::uint64_t{0}}}}));
    b.number(1020,120.0/512.0,NumericUnit::Hertz);
    b.connect(1020,20,0,frequency_step_input);
}
NumericProcessingGraph generator_example() {
    examples::Builder b;b.graph=numeric_generator_graph();set_generator_tones(b,1,1);return b.finish();
}
NumericProcessingGraph pcm_mix_example() {
    examples::Builder b;b.graph=generator_example();add_second_generator(b);
    add_mixer(b,200,1,101);replace_source(b,1,200);return b.finish();
}
NumericProcessingGraph input_selector_example() {
    examples::Builder b;b.graph=generator_example();add_second_generator(b);add_selector(b,1,2);
    connect_generator_stream(b,1);b.connect(101,200,0,2);b.connect(101,200,sample_rate_output,3);b.derive(200);return b.finish();
}
NumericProcessingGraph output_switch_example() {
    examples::Builder b;b.graph=generator_example();b.chain(200,"Switch",1,RouteProcessingParameters{2,0,true});
    replace_source(b,1,200);
    for(auto& edge:b.graph.edges)if(edge.from==200 && edge.to==9)edge.output_port=1;
    return b.finish();
}
void copy_analysis_branch(examples::Builder& b,const NumericProcessingGraph& branch) {
    for(auto node:branch.nodes)if(node.id!=16){node.id+=100;
        if(node.id==101)std::get<SignalGeneratorProcessingParameters>(node.parameters).oscillators=example_tones(2);
        b.graph.nodes.push_back(std::move(node));}
    for(auto edge:branch.edges){edge.from+=100;edge.to+=100;b.graph.edges.push_back(edge);}
    b.configuration(16,101);
}
NumericProcessingGraph independent_example() {
    examples::Builder b;b.graph=generator_example();copy_analysis_branch(b,generator_example());return b.finish();
}
NumericProcessingGraph spectral_mix_example() {
    examples::Builder b;b.graph=generator_example();add_second_generator(b);
    examples::add_window(b,101,102,103,1005,1004,4096);
    b.chain(104,"StereoFFT",1004);b.chain(115,"BarkWeighting",104,regular_spectrum_grid(48000,4096));
    add_mixer(b,200,15,115);replace_source(b,15,200);return b.finish();
}
}
domain::NumericProcessingGraph numeric_default_graph(std::uint32_t sample_rate) {
    examples::Builder b({sample_rate,4096,256,400});b.add(1,"AudioSource");examples::add_window(b,1,2,3,1000,1001,4096);
    b.chain(4,"StereoFFT",1001);b.chain(15,"BarkWeighting",4,regular_spectrum_grid(sample_rate,4096));
    b.chain(6,"Mel",15,regular_spectrum_grid(sample_rate,4096));
    b.chain(7,"Spectrogram",6);b.chain(8,"AudioSurfaceView",7);b.chain(9,"AudioLineView",1);
    b.chain(10,"AudioLineView",15);b.chain(11,"AudioLineView",6);b.chain(12,"ScalarMetricsView",7,{},1);
    examples::add_frequency_step(b,1,1000,1003);b.connect(1003,10,0,domain::frequency_step_input);return b.finish();
}
domain::NumericProcessingGraph numeric_test_graph() {
    auto graph=numeric_default_graph();auto& source=graph.nodes.front();source.kind="TestSignal";
    source.parameters=domain::EndpointProcessingParameters{{{"tones",signal_contracts::NumericLiteral{std::uint64_t{8}}},{"sample_rate",signal_contracts::NumericLiteral{std::uint64_t{48000}}},
        {"minimum_hz",signal_contracts::NumericLiteral{73.0}},{"maximum_hz",signal_contracts::NumericLiteral{13901.0}},{"amplitude",signal_contracts::NumericLiteral{0.4}},
        {"period_seconds",signal_contracts::NumericLiteral{6.0}},{"sweep_depth",signal_contracts::NumericLiteral{0.18}}}};
    return finish_example(std::move(graph));
}
domain::NumericProcessingGraph numeric_generator_graph(std::uint32_t sample_rate) {
    examples::Builder b({sample_rate,4096,256,400});b.graph=numeric_default_graph(sample_rate);auto& source=b.node(1);source.kind="SignalGenerator";
    source.parameters=domain::SignalGeneratorProcessingParameters{};source.clock_node=16;b.add(16,"SampleClock");b.configuration(16,1);return b.finish();
}
domain::NumericProcessingGraph numeric_temporal_fft_graph(std::uint32_t sample_rate) {
    examples::Builder b({sample_rate,512,256,400});b.graph=numeric_generator_graph(sample_rate);add_temporal_source(b);add_temporal_transform(b);return b.finish();
}
namespace {
void add_startup_sources(examples::Builder& b) {
    using namespace domain;
    b.add(1,"SignalGenerator");
    b.add(16,"SampleClock");
    b.node(1).clock_node=16;
    b.configuration(16,1);
    std::get<SignalGeneratorProcessingParameters>(b.node(1).parameters).oscillators=example_tones(1);
    b.add(13,"AudioSource",EndpointProcessingParameters{{{"source",signal_contracts::NumericLiteral{std::int64_t{0}}}}});
    b.add(200,"InputSelector",examples::transform({{"selected_input",signal_contracts::NumericLiteral{std::uint64_t{1}}},
        {"channels_per_stream",signal_contracts::NumericLiteral{std::uint64_t{2}}}}));
    const auto pcm=b.output(1),rate=b.output(1,sample_rate_output);
    b.node(200).inputs.clear();
    b.node(200).outputs={{0,PortDirection::Output,pcm},{1,PortDirection::Output,rate}};
    unsigned lane=0;
    for(const auto source:{1,13}) {
        b.connect(source,200,0,PortId(lane++));
        b.connect(source,200,sample_rate_output,PortId(lane++));
    }
    b.derive(200);
}
void add_startup_spectrum(examples::Builder& b) {
    using namespace domain;
    b.chain(332,"PcmResampler",200,examples::transform({{"mode",std::string{"speex"}},
        {"output_rate",signal_contracts::NumericLiteral{std::uint64_t{32768}}}}));
    b.connect(200,332,1,input_sample_rate_input);
    examples::add_window(b,332,2,3,1000,1001,4096);
    b.connect(332,2,sample_rate_output,input_sample_rate_input);
    b.chain(4,"RealFourier",1001);
    b.chain(5,"AmplitudePhase",4,examples::transform({
        {"sample_rate",signal_contracts::NumericLiteral{std::uint64_t{32768}}}}));
    b.connect(4,5,1,1);
    b.connect(3,5,coherent_gain_output,coherent_gain_input);
    b.connect(3,5,power_gain_output,power_gain_input);
    b.connect(332,5,sample_rate_output,target_sample_rate_input);
    examples::add_frequency_step(b,332,1000,1003);
}
void add_startup_correction(examples::Builder& b) {
    using namespace domain;
    b.chain(1004,"ArrayLength",5);
    b.chain(1005,"ArrayFill",1004,examples::transform({{"value",signal_contracts::NumericLiteral{1.0}},
        {"values",domain::NumericArray{startup_profile::coefficients()}},
        {"size",signal_contracts::NumericLiteral{std::uint64_t{2048}}},
        {"first_bin",signal_contracts::NumericLiteral{std::uint64_t{1}}},
        {"sample_rate",signal_contracts::NumericLiteral{std::uint64_t{32768}}},
        {"fft_size",signal_contracts::NumericLiteral{std::uint64_t{4096}}},
        {"window_function",std::string{"hann"}},
        {"representation",std::string{"peak_amplitude"}},
        {"resampler_mode",std::string{"speex"}},
        {"frontend_declared",true}}));
    b.add(15,"ArrayMultiply");
    b.connect(5,15);
    b.connect(1005,15,0,1);
    b.derive(15);
}
void add_startup_features(examples::Builder& b) {
    using namespace domain;
    b.chain(6,"BroadbandRow",15,examples::transform({{"input_representation",std::string{"peak_amplitude"}},
        {"bin_hz",signal_contracts::NumericLiteral{8.0}},
        {"peak_weight",std::string{"squared_value"}},
        {"low_reduction",std::string{"maximum"}},
        {"low_count",signal_contracts::NumericLiteral{std::uint64_t{24}}},
        {"mid_count",signal_contracts::NumericLiteral{std::uint64_t{8}}},
        {"high_count",signal_contracts::NumericLiteral{std::uint64_t{16}}},
        {"first_bin",signal_contracts::NumericLiteral{std::uint64_t{1}}}}));
    b.chain(1006,"NoteSpectrum",15,examples::transform({{"input_representation",std::string{"peak_amplitude"}},
        {"bin_hz",signal_contracts::NumericLiteral{8.0}},
        {"first_midi",signal_contracts::NumericLiteral{std::uint64_t{36}}},
        {"last_midi",signal_contracts::NumericLiteral{std::uint64_t{83}}},
        {"peak_weight",std::string{"squared_value"}},
        {"first_bin",signal_contracts::NumericLiteral{std::uint64_t{1}}}}));
    b.connect(1003,6,0,frequency_step_input);
    b.connect(1003,1006,0,frequency_step_input);
    b.add(1007,"ArrayConcat");
    b.connect(6,1007);
    b.connect(1006,1007,0,1);
    b.derive(1007);
    // The existing concatenated broadband + note row drives one 512x96 history.
    // A standard 8-bit quantizer adapts Float32 levels for the unchanged rhythm kernel.
    b.chain(1111,"SpectralQuantize",1007,examples::transform({
        {"bits",signal_contracts::NumericLiteral{std::uint64_t{8}}},
        {"min_value",signal_contracts::NumericLiteral{0.}},
        {"max_value",signal_contracts::NumericLiteral{1.}}}));
    b.chain(7,"Spectrogram",1111,domain::HistoryProcessingParameters{512});
    b.chain(8,"AudioSurfaceView",7);
    b.chain(1008,"Spectrogram",6);
    b.chain(1009,"AudioSurfaceView",1008);
    b.chain(1010,"Spectrogram",1006);
    b.chain(1011,"AudioSurfaceView",1010);
}
void add_startup_views(examples::Builder& b) {
    using namespace domain;
    b.chain(9,"AudioLineView",332);
    b.chain(10,"AudioLineView",15,examples::transform({{"first_bin",signal_contracts::NumericLiteral{std::uint64_t{1}}}}));
    b.connect(1003,10,0,frequency_step_input);
    b.chain(11,"PianoView",1006);
    b.connect(1006,11,note_first_output,note_first_input);
    b.chain(12,"ScalarMetricsView",1006,{},1);
    b.connect(1006,12,2,1);
}
}
domain::NumericProcessingGraph numeric_startup_graph() {
    examples::Builder builder;
    add_startup_sources(builder);
    add_startup_spectrum(builder);
    add_startup_correction(builder);
    add_startup_features(builder);
    add_startup_views(builder);
    append_startup_rhythm(builder);
    builder.graph.execution_interval={1,60};
    return builder.finish();
}

namespace {
std::array<std::size_t,4> section_counts(const NumericProcessingGraph& graph,NodeId id) {
    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& n){return n.id==id;});
    if(node==graph.nodes.end() || node->inputs.empty())throw std::runtime_error("Missing example section producer");
    const auto data=node->inputs.front().contract.view();const auto* input=std::get_if<NumericContract>(&data);
    std::array<OwnedProcessingContract,4> outputs;std::array<std::size_t,4> counts{};
    if(!input || musical_spectrogram_output_contracts(*node,*input,outputs,&counts)!=Status::ok)throw std::runtime_error("Invalid example section layout");
    return counts;
}
void add_startup_sections(NumericExampleGraph& example) {
    const auto bands=section_counts(example.graph,6),notes=section_counts(example.graph,1006);
    static constexpr const char* names[]{"Low","Mid","High"};std::size_t begin=0;
    for(unsigned i=0;i<3;++i){example.sections.push_back({8,names[i],begin,bands[i]});example.sections.push_back({1009,names[i],begin,bands[i]});begin+=bands[i];}
    example.sections.push_back({8,"Notes",begin,notes[3]});example.sections.push_back({1011,"Notes",0,notes[3]});
}
}
NumericExampleGraph numeric_startup_example() {
    NumericExampleGraph result{"startup","Spectrogram and rhythm: Loopback",numeric_startup_graph(),{8,1009,1011},{},{{200,{"Generator","Loopback"}}}};
    result.scalar_names.push_back({1103,{"Candidate tempo, BPM","Accepted tempo, BPM",
        "Decision score (not probability)","Tempo status: 0 unknown, 1 ambiguous, 2 determined",
        "Beat phase, 0...1","Next beat, source seconds","Tempo change, BPM/s","Spectral activity",
        "Periodicity support","ACF cost units (not time)","Accent strength",
        "Grid status: 0 unknown, 1 ambiguous, 2 determined"}});
    add_startup_sections(result);return result;
}
std::vector<NumericExampleGraph> numeric_example_graphs() {
    return {
        numeric_startup_example(),
        []{auto result=numeric_startup_example();result.id="rhythm-research";
            result.title_key="Spectrogram and rhythm: shared 96-band history";return result;}(),
        {"music-research","Music spectrum: RFFT, profiles and note salience",numeric_music_graph(),{8},{},{{200,{"Generator","Loopback"}}}},
        {"stereo-sources","Stereo: generator, loopback, microphone",source_selector_graph(2),{8},{},{{200,{"Generators","Loopback","Microphone"}}}},
        {"pcm-mix","Two channels: PCM mixer",pcm_mix_example(),{8}},
        {"input-selector","Two channels: input selector",input_selector_example(),{8},{},{{200,{"Generator A","Generator B"}}}},
        {"output-switch","Generator: output switch",output_switch_example(),{8}},
        {"two-streams","Two channels: independent analysis",independent_example(),{8,108}},
        {"spectral-mix","Two channels: spectrum mixer",spectral_mix_example(),{8}},
        {"temporal-fft","Mel bands: transpose and row FFT",numeric_temporal_fft_graph(),{8,18},{},{{200,{"Generator","Loopback"}}}}
    };
}
