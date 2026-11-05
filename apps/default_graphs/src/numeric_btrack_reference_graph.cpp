// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_btrack_reference_graph.h>
#include <core_app/numeric_default_graph.h>
#include <rhythm_graph_adapters/btrack_composition.h>
#include "example_builder.h"
#include <stdexcept>

namespace {
using namespace domain;
using signal_contracts::NumericLiteral;
TransformProcessingParameters source_parameters(std::string_view kind) {
    auto result=examples::transform({{"sample_rate",NumericLiteral{std::uint64_t{44100}}},{"hop_samples",NumericLiteral{std::uint64_t{512}}},
        {"frame_samples",NumericLiteral{std::uint64_t{1024}}},{"history_size",NumericLiteral{std::uint64_t{512}}},
        {"alpha",NumericLiteral{.9f}},{"tightness",NumericLiteral{5.f}}});
    if(kind=="BTrackWindowProduct")result.values.push_back({"window_shape",std::string{"hann"}});
    if(kind=="BTrackOnset")result.values.push_back({"cartesian_input",true});
    return result;
}
void stage(examples::Builder& b,NodeId id,std::string_view kind,NodeId first,PortId output=0,
    NodeId second=0,PortId second_output=0) {
    std::vector<ProcessingPort> inputs{{0,PortDirection::Input,b.output(first,output)}};
    if(second)inputs.push_back({1,PortDirection::Input,b.output(second,second_output)});
    ConfiguredProcessingNode node;
    const auto parameters=source_parameters(kind);
    if(rhythm_graph_adapters::make_btrack_operation_node(id,kind,inputs,parameters,b.limits.frame,
        b.limits.max_owned_metadata_bytes,node)!=signal_contracts::Status::ok)throw std::runtime_error("BTrack reference stage contract");
    b.graph.nodes.push_back(std::move(node));b.connect(first,id,output);
    if(second)b.connect(second,id,second_output,1);
}
void window_product(examples::Builder& b) {
    b.chain(20,"Window",3,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}}}));
    b.chain(21,"ArrayLength",20);
    // The extension boundary owns the selected logical window, including the
    // zero-left prefix; borrowed circular Window views cannot be latched.
    b.chain(22,"FrameStride",20,examples::transform({{"every_n",NumericLiteral{std::uint64_t{1}}},{"materialize",true}}));
    stage(b,23,"BTrackWindowProduct",22);
}
void frontend(examples::Builder& b,std::uint32_t input_rate) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},
        {"output_count",NumericLiteral{std::uint64_t{1}}}}});
    b.chain(2,"PcmResampler",1,examples::transform({{"input_rate",NumericLiteral{std::uint64_t(input_rate)}},
        {"output_rate",NumericLiteral{std::uint64_t{44100}}},{"mode",std::string{"existing_fir"}}}));
    b.connect(1,2,sample_rate_output,input_sample_rate_input);
    b.chain(3,"FrameBuffer",2,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"hop_samples",NumericLiteral{std::uint64_t{512}}}}));
    window_product(b);
    NumericArray permutation;permutation.values.resize(1024);
    for(std::size_t i=0;i<1024;++i)permutation.values[i]=float((i+512)%1024);
    b.chain(24,"ArrayReindex",23,examples::transform({{"indices",std::move(permutation)}}));
    b.chain(25,"Fourier",24,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},
        {"backend_id",std::string{"kiss_fft130_float32"}}}));
    // An unwired imaginary plane is typed zero, with the real plane's explicit
    // geometry/clock rather than the factory's unrelated generic defaults.
    b.node(25).inputs[1].contract=b.output(24);
    b.connect(21,25,0,window_size_input);
}
void polar(examples::Builder& b) {
    b.add(26,"AmplitudePhase",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},
        {"sample_rate",NumericLiteral{std::uint64_t{44100}}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"positive_half",false},{"normalize",false},
        {"representation",std::string{"magnitude"}},{"real_endpoints",true},
        {"magnitude_norm",std::string{"squared_norm"}},{"zero_phase",std::string{"atan2"}}}));
    b.connect(25,26,0,0);b.connect(25,26,1,1);b.derive(26);b.connect(21,26,0,window_size_input);
    b.connect(2,26,sample_rate_output,target_sample_rate_input);
    stage(b,30,"BTrackOnset",25,0,25,1);
}
void tracking(examples::Builder& b) {
    stage(b,31,"BTrackHistory",30);
    stage(b,32,"BTrackHistoryResample",31,1);
    stage(b,33,"BTrackCombObservation",32);
    stage(b,34,"BTrackBeatDecision",31,0,33,0);
}
void views(examples::Builder& b) {
    b.chain(40,"VectorLineView",20);b.chain(41,"VectorLineView",26);
    b.chain(42,"VectorLineView",31,{},1);b.chain(43,"VectorLineView",32);
    b.chain(44,"VectorLineView",33,{},2);b.chain(45,"VectorLineView",33,{},1);
    b.chain(46,"VectorLineView",33);b.chain(47,"VectorLineView",34,{},7);
    b.chain(48,"ScalarMetricsView",34,{},1);b.chain(49,"VectorLineView",34,{},4);
    b.chain(35,"Window",34,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}}}));
    b.chain(50,"VectorLineView",35);
    b.chain(51,"ScalarMetricsView",30);
}
void layout(NumericExampleGraph& graph) {
    graph.positions={{1,0,0},{2,450,0},{3,900,0},{20,1350,0},{21,1350,500},{22,1800,500},{23,2250,0},
        {24,2700,0},{25,3150,0},{26,3600,0},{30,4050,0},{31,4500,0},{32,4950,0},{33,5400,0},{34,5850,0},{35,6300,800},
        {40,1350,-400},{41,3600,-400},{42,4500,-400},{43,4950,-400},{44,5400,-800},{45,5400,-400},
        {46,5400,400},{47,5850,-400},{48,6300,0},{49,6300,400},{50,6750,800},{51,4050,-400}};
    graph.node_names={{40,"BTrack PCM frame"},{41,"BTrack raw magnitude"},{42,"BTrack seeded ODF history"},
        {43,"BTrack sinc ODF history"},{44,"BTrack balanced ACF"},{45,"BTrack comb response"},{46,"BTrack tempo observations"},
        {47,"BTrack tempo state"},{48,"Tempo, BPM"},{49,"BTrack cumulative scores"},{50,"BTrack beat decisions"},{51,"BTrack onset"}};
    graph.scalar_names={{48,{"Tempo, BPM"}},{51,{"BTrack onset"}}};
}
}
NumericExampleGraph numeric_btrack_reference_example(std::uint32_t input_rate) {
    if(input_rate<8000 || input_rate>96000)throw std::invalid_argument("BTrack input rate invalid");
    examples::Builder b({input_rate,1024,512,400});b.graph.graph_id="reference-btrack";
    frontend(b,input_rate);polar(b);tracking(b);views(b);
    NumericExampleGraph graph{"reference-btrack","Rhythm: BTrack reference pipeline",b.finish()};layout(graph);return graph;
}
