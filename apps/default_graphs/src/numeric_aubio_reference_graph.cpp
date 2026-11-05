// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_default_graph.h>
#include "example_builder.h"
#include <rhythm_graph_adapters/composition.h>
#include <rhythm_graph_adapters/descriptor_composition.h>
#include <rhythm_graph_adapters/event_composition.h>
#include <rhythm_graph_adapters/pcm_stage_composition.h>
#include <rhythm_reference_ports/reference_window.h>
#include <stdexcept>

namespace {
using namespace domain;using namespace signal_contracts;
using Maker=Status (*)(NodeId,std::string_view,std::span<const ProcessingPort>,TransformProcessingParameters,const FrameLimits&,std::size_t,ConfiguredProcessingNode&);
struct Link {NodeId node;PortId output=0,input=0;};
void stage(examples::Builder& b,NodeId id,std::string_view kind,Maker make,TransformProcessingParameters parameters,std::initializer_list<Link> links) {
    std::vector<ProcessingPort> inputs;inputs.reserve(links.size());
    for(const auto& link:links)inputs.push_back({link.input,PortDirection::Input,b.output(link.node,link.output)});
    ConfiguredProcessingNode node;
    if(make(id,kind,inputs,std::move(parameters),b.limits.frame,b.limits.max_owned_metadata_bytes,node)!=Status::ok)
        throw std::runtime_error("Aubio graph stage: "+std::string(kind));
    b.graph.nodes.push_back(std::move(node));
    for(const auto& link:links)b.connect(link.node,id,link.output,link.input);
}
NumericArray indices(std::size_t size,std::size_t shift=0) {
    NumericArray result;result.values.reserve(size);
    for(std::size_t i=0;i<size;++i)result.values.push_back(float((i+shift)%size));
    return result;
}
NumericArray source_window() {
    NumericArray values;values.values.resize(1024);
    if(!rhythm_reference_ports::analysis_window(values.values,rhythm_reference_ports::AnalysisWindowShape::PeriodicHann))throw std::runtime_error("Aubio window");
    return values;
}
void frontend(examples::Builder& b) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},{"output_count",NumericLiteral{std::uint64_t{1}}}}});
    b.chain(2,"PcmResampler",1,examples::transform({{"mode",std::string{"existing_fir"}},{"output_rate",NumericLiteral{std::uint64_t{44100}}}}));
    b.connect(1,2,sample_rate_output,input_sample_rate_input);
    b.chain(3,"FrameBuffer",2,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"hop_samples",NumericLiteral{std::uint64_t{512}}}}));
    b.chain(4,"Window",3,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}}}));
    b.chain(5,"ArrayLength",4);
    b.chain(6,"ArrayFill",5,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}},{"values",source_window()}}));
    b.add(7,"ArrayMultiply");b.connect(4,7);b.connect(6,7,0,1);b.derive(7);
    b.chain(8,"ArrayReindex",7,examples::transform({{"indices",indices(1024,512)}}));
    b.chain(10,"RealFourier",8,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},
        {"backend_id",std::string{"aubio_ooura_fft8g_float32"}},{"include_endpoints",true}}));
}
void polar(examples::Builder& b) {
    b.add(11,"AmplitudePhase",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},{"first_bin",NumericLiteral{std::uint64_t{0}}},
        {"sample_rate",NumericLiteral{std::uint64_t{44100}}},{"positive_half",true},{"normalize",false},
        {"representation",std::string{"magnitude"}},{"magnitude_norm",std::string{"squared_norm"}},{"zero_phase",std::string{"atan2"}},{"real_endpoints",true}}));
    b.connect(10,11);b.connect(10,11,1,1);b.derive(11);
    b.connect(6,11,0,window_coefficients_input);
    b.chain(12,"ArrayReindex",11,examples::transform({{"indices",indices(513)}}));
    b.chain(13,"ArrayReindex",11,examples::transform({{"indices",indices(513)}}),1);
}
void onset(examples::Builder& b) {
    using namespace rhythm_graph_adapters;
    stage(b,20,"OnsetMagnitude",make_spectral_operation_node,examples::transform({{"whiten",false},{"compression",NumericLiteral{1.f}},
        {"frame_period_s",NumericLiteral{512.f/44100.f}}}),{{12}});
    stage(b,21,"OnsetDescriptor",make_descriptor_operation_node,examples::transform({{"method",NumericLiteral{std::uint64_t{1}}}}),{{20}});
    stage(b,22,"AdaptivePeakPicker",make_event_operation_node,examples::transform({{"threshold",NumericLiteral{.058f}}}),{{21}});
    stage(b,23,"PcmSilence",make_event_operation_node,examples::transform({{"threshold_db",NumericLiteral{-70.f}}}),{{3}});
    stage(b,24,"OnsetDecision",make_event_operation_node,examples::transform({{"hop_samples",NumericLiteral{std::uint64_t{512}}},
        {"delay_samples",NumericLiteral{std::uint64_t{2201}}},{"minimum_interval_samples",NumericLiteral{std::uint64_t{2205}}}}),{{22},{23,0,1}});
}
void tempo(examples::Builder& b) {
    using namespace rhythm_graph_adapters;
    stage(b,30,"OnsetDescriptor",make_descriptor_operation_node,examples::transform({{"method",NumericLiteral{std::uint64_t{7}}}}),{{12}});
    stage(b,31,"AdaptivePeakPicker",make_event_operation_node,examples::transform({{"threshold",NumericLiteral{.3f}}}),{{30}});
    stage(b,32,"PcmSilence",make_event_operation_node,examples::transform({{"threshold_db",NumericLiteral{-90.f}}}),{{3}});
    stage(b,33,"ContextBeatSequence",make_pcm_stage_node,examples::transform({{"hop_samples",NumericLiteral{std::uint64_t{512}}},
        {"frame_rate",NumericLiteral{44100.f/512.f}},{"tracker_window",NumericLiteral{std::uint64_t{512}}},
        {"delay_samples",NumericLiteral{std::int64_t{0}}},{"tatums_per_beat",NumericLiteral{std::uint64_t{4}}},{"poll_tatum",true}}),{{31,2},{32,0,1}});
}
void view(examples::Builder& b,NodeId id,NodeId source,PortId port=0,bool scalar=false) {
    b.chain(id,scalar?"ScalarMetricsView":"VectorLineView",source,{},port);
}
void views(examples::Builder& b) {
    view(b,100,4);view(b,101,12);view(b,102,13);view(b,103,20);
    b.chain(40,"Window",21,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}}}));view(b,104,40);
    b.chain(41,"Window",31,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}}}),2);view(b,105,41);
    view(b,106,24,1,true);view(b,107,24,2,true);view(b,108,33,4,true);
    view(b,109,33,6,true);view(b,110,33,2,true);view(b,111,33,10);view(b,112,33,9);view(b,113,33,8,true);view(b,114,33,7,true);
}
void describe(NumericExampleGraph& graph) {
    graph.node_names={{100,"Aubio PCM window"},{101,"Aubio raw magnitude"},{102,"Aubio phase"},{103,"Aubio compressed magnitude"},
        {104,"Aubio HFC history"},{105,"Aubio thresholded flux history"},{106,"Aubio onset sample"},{107,"Aubio onset detected"},
        {108,"Aubio tempo, BPM"},{109,"Aubio beat sample"},{110,"Aubio source tactus"},{111,"Aubio autocorrelation"},{112,"Aubio tracker history"},{113,"Aubio tatum poll"},{114,"Aubio tatum sample"}};
    for(std::size_t i=0;i<graph.graph.nodes.size();++i)graph.positions.push_back({graph.graph.nodes[i].id,double(i%8)*440,double(i/8)*420});
}
}
NumericExampleGraph numeric_aubio_reference_example() {
    examples::Builder b({44100,1024,512,512});b.graph.graph_id="aubio-reference";
    frontend(b);polar(b);onset(b);tempo(b);views(b);
    NumericExampleGraph result{"aubio-reference","Aubio: default onset and tempo",b.finish()};describe(result);return result;
}
