#include <core_app/numeric_default_graph.h>
#include "example_builder.h"
#include "compact_spectrogram_frequency_profile.h"
#include <dsp_kernels/musical_features.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace {
using namespace domain;
using namespace signal_contracts;
constexpr std::uint64_t fft_size=4096,rate=36000;
constexpr std::uint64_t capture_packet_samples=1024;
constexpr double bin_hz=double(rate)/fft_size;
constexpr unsigned first_note_midi=48,last_note_midi=95;
struct RhythmSection {const char* label;double low,high;unsigned count;};
constexpr RhythmSection rhythm_sections[]{
    {"Bass",40,250,12},{"Density",250,800,4},
    {"Impact",800,1500,6},{"Click",1500,3500,8},
    {"Noise",3500,6000,6},{"Rustle",6000,10000,4},
    {"Air",10000,16000,8}};
NumericArray rhythm_boundaries() {
    NumericArray result;result.values.push_back(40);
    for(const auto& section:rhythm_sections)for(unsigned i=1;i<=section.count;++i) {
        const double fraction=double(i)/section.count;
        const double value=std::lerp(section.low,section.high,fraction);
        result.values.push_back(float(i==section.count?section.high:value));
    }
    return result;
}
void rhythm_labels(NumericExampleGraph& result) {
    for(const auto view:{8,9}) {
        size_t offset=0;
        for(const auto& section:rhythm_sections){result.sections.push_back({view,section.label,offset,section.count});offset+=section.count;}
        result.sections.push_back({view,"Notes",48,48});
    }
}
void spectrogram_positions(NumericExampleGraph& result) {
    result.positions={
        {1,-39.91975236368384,313.79585161063704},
        {2,2207.9343034025947,-96.83950472736765},
        {3,3262.8686068051884,-35.85405576627784},
        {4,4473.032848298702,155.23368431180356},
        {5,5064.32848298703,193.85780198716049},
        {7,8722,17},
        {8,9210,17},
        {9,9210,287},
        {10,9210,650},
        {11,9670,650},
        {12,10130,650},
        {13,-3.328482987029929,-315.38006239014607},
        {14,-9.427027883138948,-34.953034507007146},
        {15,6517,17},
        {16,17,835},
        {31,7066.919752363683,12.934303402594011},
        {32,7010,253},
        {33,8279,17},
        {35,7827,17},
        {200,528,17},
        {201,1051.9671517012966,-19.591269376653898},
        {202,1522,17},
        {203,2813.1642414935154,-147.6607121949425},
        {1000,2803,199},
        {1001,3990.1970897922183,-165.95634688326942},
        {1003,3053.48523203878,530.2848298702993},
        {1004,5760,17},
        {1005,6003.967151701297,-157.82495368845744}};
}
void converter(examples::Builder& b,NodeId id,NodeId from,bool integer) {
    b.chain(id,"Converter",from);
    auto data=b.output(from).view();auto output=std::get<NumericContract>(data);
    output.dtype=integer?DType::Int16:DType::Float32;
    output.representation.normalization=integer?Normalization::SignedUnit:Normalization::FloatUnit;
    output.representation.alignment=integer?alignof(std::int16_t):alignof(float);
    auto& policy=std::get<ConverterProcessingParameters>(b.node(id).parameters).policy;
    policy.scaling.mode=integer?ScaleMode::PcmDenormalize:ScaleMode::PcmNormalize;
    policy.precision_loss=PrecisionLoss::AllowDeclared;policy.overflow=Overflow::Saturate;
    if(own_processing_contract(output,b.limits.frame,b.limits.max_owned_metadata_bytes,b.node(id).outputs[0].contract)!=Status::ok)
        throw std::runtime_error("Spectrogram PCM conversion contract");
    b.derive(id);
}
void normalized_source(examples::Builder& b,NodeId id) {
    auto value=b.output(id).view();auto& pcm=std::get<NumericContract>(value);
    pcm.unit=NumericUnit::NormalizedAmplitude;pcm.representation.normalization=Normalization::FloatUnit;
    pcm.shape.extents[0].minimum=1;
    if(id!=1)pcm.shape.extents[0].maximum=capture_packet_samples;
    if(own_processing_contract(value,b.limits.frame,b.limits.max_owned_metadata_bytes,b.node(id).outputs[0].contract)!=Status::ok)
        throw std::runtime_error("Spectrogram normalized source contract");
}
OwnedProcessingContract selector_envelope(examples::Builder& b) {
    auto envelope=b.output(13).view();std::get<NumericContract>(envelope).shape.extents[0]={1,capture_packet_samples};
    OwnedProcessingContract pcm;
    if(own_processing_contract(envelope,b.limits.frame,b.limits.max_owned_metadata_bytes,pcm)!=Status::ok)throw std::runtime_error("Spectrogram selector envelope");
    return pcm;
}
void selector(examples::Builder& b) {
    for(const auto id:{1,13,14})normalized_source(b,id);
    b.add(200,"InputSelector",examples::transform({{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}));
    const auto pcm=selector_envelope(b);
    b.node(200).inputs.clear();b.node(200).outputs={{0,PortDirection::Output,pcm},{1,PortDirection::Output,b.output(13,sample_rate_output)}};
    unsigned stream=0;
    for(const auto source:{13,14,1}) {
        b.connect(source,200,0,PortId(stream*2));b.connect(source,200,sample_rate_output,PortId(stream*2+1));++stream;
    }
    b.derive(200);
}
void sources(examples::Builder& b) {
    b.add(1,"SignalGenerator");b.add(16,"SampleClock");b.node(1).clock_node=16;b.configuration(16,1);
    b.add(13,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::int64_t{0}}},{"output_count",NumericLiteral{std::uint64_t{1}}}}});
    b.add(14,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::int64_t{1}}},{"output_count",NumericLiteral{std::uint64_t{1}}}}});
    selector(b);
}
void fourier(examples::Builder& b) {
    b.chain(4,"RealFourier",1001);
    b.connect(1000,4,0,window_size_input);
    b.add(5,"AmplitudePhase",examples::transform({{"representation",std::string{"peak_amplitude"}},{"sample_rate",NumericLiteral{rate}},{"real_endpoints",true}}));
    b.connect(4,5);b.connect(4,5,1,1);b.derive(5);
    b.connect(1000,5,0,window_size_input);
    b.connect(3,5,0,window_coefficients_input);
    b.connect(202,5,sample_rate_output,target_sample_rate_input);
}
void frontend(examples::Builder& b) {
    converter(b,201,200,true);
    b.chain(202,"PcmResampler",201,examples::transform({{"mode",std::string{"speex_q15"}},{"output_rate",NumericLiteral{rate}},{"quality",NumericLiteral{std::uint64_t{5}}}}));
    b.connect(200,202,1,input_sample_rate_input);
    b.chain(2,"FrameBuffer",202,examples::transform({{"size",NumericLiteral{fft_size}},{"hop_samples",NumericLiteral{std::uint64_t{600}}}}));
    converter(b,203,2,false);
    b.chain(1000,"ArrayLength",2);
    b.chain(3,"WindowFunction",1000,examples::transform({{"size",NumericLiteral{fft_size}},{"function",std::string{"hann"}},{"periodic",true}}));
    b.add(1001,"ArrayMultiply");b.connect(203,1001);b.connect(3,1001,0,1);b.derive(1001);
    fourier(b);
    examples::add_frequency_step(b,202,1000,1003);
}
void compensation(examples::Builder& b) {
    b.chain(1004,"ArrayLength",5);
    b.chain(1005,"ArrayFill",1004,examples::transform({{"values",NumericArray{{spectrogram_profile::kBillboardGain36k.begin()+1,spectrogram_profile::kBillboardGain36k.end()}}},
        {"size",NumericLiteral{std::uint64_t{2048}}}}));
    b.add(15,"ArrayMultiply",examples::transform({{"frame_input",NumericLiteral{std::uint64_t{1}}}}));
    b.connect(1005,15);b.connect(5,15,0,1);b.derive(15);
}
void features(examples::Builder& b) {
    b.chain(31,"BroadbandFeatures",15,examples::transform({{"statistic",std::string{"maximum"}},{"boundaries_hz",rhythm_boundaries()},
        {"first_bin",NumericLiteral{std::uint64_t{1}}},{"bin_hz",NumericLiteral{bin_hz}}}));
    b.connect(1003,31,0,frequency_step_input);
    b.chain(32,"NoteSpectrum",15,examples::transform({{"first_midi",NumericLiteral{std::uint64_t{first_note_midi}}},{"last_midi",NumericLiteral{std::uint64_t{last_note_midi}}},
        {"first_bin",NumericLiteral{std::uint64_t{1}}},{"bin_hz",NumericLiteral{bin_hz}},{"input_representation",std::string{"peak_amplitude"}}}));
    b.connect(1003,32,0,frequency_step_input);
    b.add(35,"ArrayConcat");b.connect(31,35);b.connect(32,35,0,1);b.derive(35);
    b.chain(33,"SpectralQuantize",35,examples::transform({{"bits",NumericLiteral{std::uint64_t{8}}},{"min_value",NumericLiteral{0.}},{"max_value",NumericLiteral{1.}}}));
    b.chain(7,"Spectrogram",33,HistoryProcessingParameters{512});
    b.chain(8,"CompactSpectrogramView",7);
    b.chain(9,"AudioSurfaceView",7);
    b.chain(10,"SpectralQuantize",15,examples::transform({{"bits",NumericLiteral{std::uint64_t{8}}},{"min_value",NumericLiteral{0.}},{"max_value",NumericLiteral{1.}}}));
    b.chain(11,"Spectrogram",10,HistoryProcessingParameters{64});
    b.chain(12,"ClassicSpectrogramView",11);
}
}
domain::NumericProcessingGraph numeric_compact_spectrogram_graph() {
    examples::Builder b({48000,4096,256,512});b.graph.execution_interval={1,60};
    sources(b);frontend(b);compensation(b);features(b);return b.finish();
}
NumericExampleGraph numeric_compact_spectrogram_example() {
    NumericExampleGraph result{"compact-spectrogram","Compact spectrogram: frequencies and notes",numeric_compact_spectrogram_graph(),{8,9,12},
        {},{{200,{"Loopback","Microphone","Generator"}}}};
    rhythm_labels(result);spectrogram_positions(result);
    const auto edges=rhythm_boundaries();const std::vector<double> frequencies(edges.values.begin(),edges.values.end());
    dsp_kernels::BroadbandFeatures layout;
    if(!layout.prepare(2048,bin_hz,frequencies,dsp_kernels::BandStatistic::Maximum,1))throw std::runtime_error("Research frequency layout");
    const auto centers=layout.centers_hz();
    result.compact_layouts.push_back({8,{centers.begin(),centers.end()},first_note_midi});
    return result;
}
