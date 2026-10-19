#include <core_app/numeric_default_graph.h>
#include <dsp_kernels/window_math.h>
#include "example_builder.h"
#include <utility>
namespace {
using namespace domain;
using signal_contracts::NumericLiteral;
constexpr std::uint32_t music_sample_rate=32768;
constexpr std::uint64_t music_fft_size=4096;

void music_sources(examples::Builder& b) {
    b.add(1,"SignalGenerator");
    b.add(16,"SampleClock");
    b.node(1).clock_node=16;
    b.configuration(16,1);
    auto& tones=std::get<SignalGeneratorProcessingParameters>(b.node(1).parameters).oscillators;
    tones.clear();
    const double frequencies[]{130.81,164.81,196.,261.63,329.63,392.,523.25,783.99};
    for(unsigned i=0;i<8;++i)tones.push_back({i+1,true,frequencies[i],.055,double(i)*.27,0,0});
    b.add(13,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::int64_t{0}}}}});
    b.add(200,"InputSelector",examples::transform({{"selected_input",NumericLiteral{std::uint64_t{0}}},
        {"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}));
    const auto pcm=b.output(1),rate=b.output(1,sample_rate_output);
    b.node(200).inputs.clear();
    b.node(200).outputs={{0,PortDirection::Output,pcm},{1,PortDirection::Output,rate}};
    b.connect(1,200);
    b.connect(1,200,sample_rate_output,1);
    b.connect(13,200,0,2);
    b.connect(13,200,sample_rate_output,3);
    b.derive(200);
}
void music_transform(examples::Builder& b) {
    b.chain(201,"PcmResampler",200,examples::transform({{"mode",std::string{"compact_fir"}},
        {"output_rate",NumericLiteral{std::uint64_t{music_sample_rate}}}}));
    b.connect(200,201,1,input_sample_rate_input);
    examples::add_window(b,201,2,3,1000,1001,music_fft_size);
    examples::add_frequency_step(b,201,1000,1003);
    b.chain(4,"RFFT",1001);
    b.chain(5,"SpectrumProjection",4,examples::transform({{"representation",std::string{"power"}},
        {"fft_size",NumericLiteral{music_fft_size}},{"sample_rate",NumericLiteral{double(music_sample_rate)}},
        {"window_function",std::string{"hann"}}}));
    b.chain(30,"SpectrumProjection",4,examples::transform({{"representation",std::string{"phase"}},
        {"fft_size",NumericLiteral{music_fft_size}},{"sample_rate",NumericLiteral{double(music_sample_rate)}},
        {"window_function",std::string{"hann"}}}));
    b.chain(6,"FrequencyNormalizer",5,examples::transform({{"mode",std::string{"off"}},
        {"sample_rate",NumericLiteral{std::uint64_t{music_sample_rate}}},{"fft_size",NumericLiteral{music_fft_size}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"window_function",std::string{"hann"}},
        {"resampler_mode",std::string{"compact_fir"}},{"frontend_declared",true}}));
}
void music_features(examples::Builder& b) {
    b.chain(31,"BroadbandFeatures",6,examples::transform({
        {"boundaries_hz",NumericArray{{0,60,120,200,300,450,650,900,1250,1750,2500,3500,5000,7000,9500,12500,16000}}},
        {"statistic",std::string{"rms"}},
        {"bin_hz",NumericLiteral{double(music_sample_rate)/double(music_fft_size)}}}));
    b.chain(32,"NoteSalience",6,examples::transform({{"first_midi",NumericLiteral{std::int64_t{48}}},
        {"notes",NumericLiteral{std::int64_t{48}}},{"a4_hz",NumericLiteral{440.0}},
        {"input_representation",std::string{"power"}},{"first_bin",NumericLiteral{std::uint64_t{0}}},
        {"include_nyquist",true},{"bin_hz",NumericLiteral{double(music_sample_rate)/double(music_fft_size)}},
        {"coherent_gain",NumericLiteral{dsp_kernels::window_sum(music_fft_size,dsp_kernels::WindowFunction::Hann)}},
        {"power_bandwidth_hz",NumericLiteral{1.5*double(music_sample_rate)/double(music_fft_size)}}}));
    b.connect(1003,32,0,frequency_step_input);
    for(const auto pair:{std::pair{31,33},std::pair{32,34}})
        b.chain(pair.second,"SpectralQuantize",pair.first,examples::transform({{"bits",NumericLiteral{std::int64_t{8}}},
            {"min_value",NumericLiteral{0.0}},{"max_value",NumericLiteral{.5}}}));
    b.chain(7,"Spectrogram",33);
    b.chain(17,"Spectrogram",34);
}
void music_views(examples::Builder& b) {
    b.chain(8,"AudioSurfaceView",7);
    b.chain(18,"AudioSurfaceView",17);
    b.chain(9,"AudioLineView",200);
    b.chain(10,"AudioLineView",5);
    b.chain(11,"AudioLineView",6);
    b.chain(12,"AudioLineView",30);
    b.chain(19,"AudioLineView",31);
    b.chain(20,"PianoView",32,EndpointProcessingParameters{{{"first_midi",NumericLiteral{std::uint64_t{48}}}}});
    b.chain(21,"ScalarMetricsView",7,{},1);
    b.chain(22,"ScalarMetricsView",17,{},1);
    for(const auto view:{10,11,12})b.connect(1003,view,0,frequency_step_input);
}
}
domain::NumericProcessingGraph numeric_music_graph() {
    examples::Builder builder;
    music_sources(builder);
    music_transform(builder);
    music_features(builder);
    music_views(builder);
    return builder.finish();
}
