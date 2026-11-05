// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_essentia_reference_graph.h>
#include "example_builder.h"
#include <rhythm_graph_adapters/essentia_composition.h>
#include <rhythm_graph_adapters/tracking_composition.h>
#include <rhythm_graph_adapters/sequence_source_composition.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/temporal_window.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
using namespace domain;using namespace signal_contracts;
struct Link {NodeId source;PortId port=0;};
using Maker=Status (*)(NodeId,std::string_view,std::span<const OwnedProcessingContract>,
    TransformProcessingParameters,const FrameLimits&,std::size_t,ConfiguredProcessingNode&);
void stage(examples::Builder& b,NodeId id,std::string_view kind,Maker make,
    TransformProcessingParameters parameters,std::initializer_list<Link> links) {
    std::vector<OwnedProcessingContract> inputs;
    for(const auto& link:links)inputs.push_back(b.output(link.source,link.port));
    ConfiguredProcessingNode node;
    if(make(id,kind,inputs,std::move(parameters),b.limits.frame,b.limits.max_owned_metadata_bytes,node)!=Status::ok)
        throw std::runtime_error("Essentia stage: "+std::string(kind));
    b.graph.nodes.push_back(std::move(node));PortId input=0;
    for(const auto& link:links)b.connect(link.source,id,link.port,input++);
}
NumericArray indices(std::size_t count,std::size_t shift=0,std::size_t modulus=2048) {
    NumericArray result;result.values.reserve(count);
    for(std::size_t i=0;i<count;++i)result.values.push_back(float((i+shift)%modulus));
    return result;
}
void pcm_extent(examples::Builder& b,NodeId id,std::size_t maximum,bool seconds) {
    auto contract=std::get<NumericContract>(b.output(id).view());
    contract.shape.extents[0]={1,maximum};
    if(seconds)contract.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,44100}};
    OwnedProcessingContract owned;
    if(own_processing_contract(contract,b.limits.frame,b.limits.max_owned_metadata_bytes,owned)!=Status::ok)
        throw std::invalid_argument("Essentia finite PCM capacity");
    b.node(id).outputs[0].contract=std::move(owned);
}
void stream_source(examples::Builder& b,bool finite=false) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},
        {"output_count",NumericLiteral{std::uint64_t{1}}}}});
    if(finite)pcm_extent(b,1,4096,false);
    b.chain(2,"PcmResampler",1,examples::transform({{"mode",std::string{"existing_fir"}},
        {"output_rate",NumericLiteral{std::uint64_t{44100}}}}));
    b.connect(1,2,sample_rate_output,input_sample_rate_input);
}
void source(examples::Builder& b) {
    stream_source(b);
    b.chain(3,"FrameBuffer",2,examples::transform({{"size",NumericLiteral{std::uint64_t{352800}}},
        {"hop_samples",NumericLiteral{std::uint64_t{352800}}}}));
}
void finite_source(examples::Builder& b,std::size_t maximum) {
    stream_source(b,true);b.add(3,"NetworkInput");pcm_extent(b,3,maximum,true);
}
void coefficients(examples::Builder& b) {
    NumericArray window;window.values.resize(2048);
    rhythm_reference_ports::SpectralWindowConfig config;
    config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    if(!rhythm_reference_ports::spectral_window(window.values,config))throw std::runtime_error("Essentia source window");
    b.add(4,"ArrayFill",examples::transform({{"size",NumericLiteral{std::uint64_t{2048}}},
        {"values",std::move(window)}}));
}
void fourier(examples::Builder& b,NodeId first) {
    b.chain(first+3,"RealFourier",first+2,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{2048}}},
        {"backend_id",std::string{"kiss_fft130_float32"}},{"include_endpoints",true}}));
}
void polar(examples::Builder& b,NodeId first) {
    b.chain(first+4,"ArrayReindex",first+3,examples::transform({{"indices",indices(1025)}}));
    b.chain(first+5,"ArrayReindex",first+3,examples::transform({{"indices",indices(1025)}}),1);
    b.add(first+6,"AmplitudePhase",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{2048}}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"sample_rate",NumericLiteral{std::uint64_t{44100}}},
        {"positive_half",true},{"normalize",false},
        {"real_endpoints",false},{"representation",std::string{"magnitude"}},
        {"magnitude_norm",std::string{"squared_norm"}},{"zero_phase",std::string{"atan2"}}}));
    b.connect(first+4,first+6);b.connect(first+5,first+6,0,1);b.derive(first+6);
}
void spectrum(examples::Builder& b,NodeId first,std::uint64_t hop) {
    b.chain(first,"TemporalFrameSequence",3,examples::transform({{"window_length",NumericLiteral{std::uint64_t{2048}}},
        {"hop",NumericLiteral{hop}},{"center",false},{"tail",NumericLiteral{std::uint64_t{1}}}}));
    if(first==10){
        b.chain(18,"ArrayLength",10,examples::transform({{"axis",NumericLiteral{std::uint64_t{1}}}}));
        b.connect(18,4);b.derive(4);
    }
    b.add(first+1,"ArrayMultiply");b.connect(first,first+1);b.connect(4,first+1,0,1);b.derive(first+1);
    b.chain(first+2,"ArrayReindex",first+1,examples::transform({{"indices",indices(2048,1024)}}));
    fourier(b,first);polar(b,first);
}
void odf(examples::Builder& b,bool multi) {
    using namespace rhythm_graph_adapters;
    stage(b,40,"EssentiaOnsetRows",make_essentia_operation_node,
        examples::transform({{"method",NumericLiteral{std::uint64_t{1}}},{"frame_rate",NumericLiteral{44100.f/1024.f}}}),{{16},{16,1}});
    if(!multi)return;
    stage(b,41,"EssentiaOnsetRows",make_essentia_operation_node,
        examples::transform({{"method",NumericLiteral{std::uint64_t{5}}},{"frame_rate",NumericLiteral{44100.f/1024.f}}}),{{16}});
    stage(b,42,"EssentiaOnsetRows",make_essentia_operation_node,
        examples::transform({{"method",NumericLiteral{std::uint64_t{4}}},{"frame_rate",NumericLiteral{44100.f/1024.f}}}),{{16}});
    stage(b,43,"EssentiaNoveltyRows",make_essentia_operation_node,{},{{26},{26,1}});
    stage(b,44,"EssentiaErbRows",make_essentia_operation_node,{},{{43}});
    stage(b,45,"BandBeatEmphasis",make_tracking_operation_node,
        examples::transform({{"frame_rate",NumericLiteral{44100.f/512.f}},{"bands",NumericLiteral{std::uint64_t{40}}}}),{{44}});
    stage(b,46,"EssentiaInformationRows",make_essentia_operation_node,
        examples::transform({{"frame_rate",NumericLiteral{44100.f/512.f}}}),{{26}});
}
void candidates(examples::Builder& b,bool multi) {
    if(multi)stage(b,47,"EssentiaCandidateInputs",rhythm_graph_adapters::make_essentia_operation_node,
        {},{{40},{41},{42},{45},{46}});
    for(std::size_t i=0;i<(multi?5u:1u);++i)
        stage(b,NodeId(50+i),"PeriodBeatTracker",rhythm_graph_adapters::make_tracking_operation_node,
            examples::transform({{"frame_rate",NumericLiteral{44100.f/(i<3?1024.f:512.f)}},
                {"upsampling",NumericLiteral{std::uint64_t(i<3?2:1)}},
                {"minimum_bpm",NumericLiteral{40.f}},{"maximum_bpm",NumericLiteral{208.f}}}),{{multi?47:40,PortId(multi?i:0)}});
}
void summary(examples::Builder& b,bool multi) {
    using namespace rhythm_graph_adapters;
    if(multi){
        stage(b,60,"EssentiaAgreement",make_essentia_operation_node,
            examples::transform({{"minimum_time",NumericLiteral{5.f}}}),{{50,1},{51,1},{52,1},{53,1},{54,1}});
        stage(b,61,"BeatIntervalSummary",make_sequence_source_node,{},{{60},{60,1}});
    }else{
        b.add(59,"Number",EndpointProcessingParameters{{{"value",NumericLiteral{0.f}}}});
        stage(b,61,"BeatIntervalSummary",make_sequence_source_node,{},{{50,1},{59}});
    }
}
void view(examples::Builder& b,NodeId id,NodeId from,PortId port=0,bool matrix=false) {
    b.chain(id,matrix?"MatrixImageView":"VectorLineView",from,{},port);
}
void primary_views(examples::Builder& b,bool finite=false) {
    if(!finite){
        b.chain(6,"Window",3,examples::transform({{"size",NumericLiteral{std::uint64_t{4096}}}}));
        b.chain(7,"MatrixRow",16,examples::transform({{"row",NumericLiteral{std::uint64_t{343}}}}));
        view(b,100,6);view(b,101,7);
    }
    view(b,102,40);view(b,103,50,1);
    view(b,104,50,2);view(b,105,61,2);view(b,106,61,3);
    b.chain(107,"ScalarMetricsView",61);b.connect(61,107,1,1);
}
void multi_views(examples::Builder& b) {
    view(b,110,41);view(b,111,42);view(b,112,45);view(b,113,46);
    view(b,114,60);view(b,115,60,3);view(b,116,60,4,true);view(b,117,44,0,true);
    for(std::size_t i=1;i<5;++i)view(b,NodeId(120+i),NodeId(50+i),1);
}
void views(examples::Builder& b,bool multi,bool finite=false) {primary_views(b,finite);if(multi)multi_views(b);}
void processing(examples::Builder& b,bool multi,bool finite=false) {
    coefficients(b);spectrum(b,10,1024);if(multi)spectrum(b,20,512);
    odf(b,multi);candidates(b,multi);summary(b,multi);views(b,multi,finite);
}
void compose(examples::Builder& b,bool multi) {
    source(b);processing(b,multi);
}
void finite_limits(examples::Builder& b,EssentiaFiniteOptions options) {
    if(!options.input_samples || options.input_samples>options.maximum_native_pcm_samples ||
       options.input_sample_rate!=44100 || options.maximum_native_pcm_samples>std::size_t(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Essentia finite input bounds or native rate");
    dsp_kernels::TemporalWindowConfig config;config.length=2048;config.hop=512;
    config.center=false;config.tail=dsp_kernels::TemporalTail::CoverEnd;
    std::size_t rows=0;
    if(!dsp_kernels::PreparedTemporalWindow::frame_count(config,options.maximum_native_pcm_samples,rows) ||
       !rows || rows>65536 || rows>SIZE_MAX/(2048*sizeof(float)))
        throw std::invalid_argument("Essentia finite analysis capacity");
    b.limits.frame.max_elements=std::max({std::size_t{2097152},options.maximum_native_pcm_samples,rows*2048});
    b.limits.frame.max_payload_bytes=b.limits.frame.max_elements*sizeof(float);
}
void describe(NumericExampleGraph& g) {
    g.node_names={{3,"Essentia complete 8 s session"},{4,"Essentia normalized symmetric Hann"},
        {100,"Essentia PCM session tail"},{101,"Essentia last raw magnitude"},{102,"Essentia complex ODF"},
        {103,"Essentia complex candidate seconds"},{104,"Essentia period seconds"},{105,"Essentia beat intervals seconds"},
        {106,"Essentia BPM observations"},{107,"Essentia BPM and confidence"},{110,"Essentia RMS ODF"},
        {111,"Essentia Mel flux ODF"},{112,"Essentia beat emphasis ODF"},{113,"Essentia information gain ODF"},
        {114,"Essentia selected beat seconds"},{115,"Essentia agreement scores"},{116,"Essentia pairwise agreement"},
        {117,"Essentia ERB distance rows"},{121,"Essentia RMS candidate seconds"},{122,"Essentia Mel candidate seconds"},
        {123,"Essentia emphasis candidate seconds"},{124,"Essentia information candidate seconds"}};
    g.scalar_names={{107,{"Tempo, BPM","Confidence"}}};
    for(std::size_t i=0;i<g.graph.nodes.size();++i)
        g.positions.push_back({g.graph.nodes[i].id,double(i%7)*480,double(i/7)*440});
}
}
NumericExampleGraph numeric_essentia_reference_example(EssentiaReferenceMethod method) {
    if(method!=EssentiaReferenceMethod::Degara && method!=EssentiaReferenceMethod::MultiFeature)
        throw std::invalid_argument("Essentia reference method");
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    examples::Builder b({44100,2048,512,512});
    // Explicit optional research batch geometry; main/SDK budgets are unchanged.
    b.limits.frame.max_elements=2097152;b.limits.frame.max_payload_bytes=8388608;
    const char* id=multi?"essentia-reference-multifeature":"essentia-reference-degara";b.graph.graph_id=id;
    compose(b,multi);
    NumericExampleGraph result{id,multi?"Essentia: RhythmExtractor2013 MultiFeature":"Essentia: RhythmExtractor2013 Degara",b.finish()};
    describe(result);return result;
}
NumericExampleGraph numeric_essentia_finite_reference_example(EssentiaFiniteOptions options,EssentiaReferenceMethod method) {
    if(method!=EssentiaReferenceMethod::Degara && method!=EssentiaReferenceMethod::MultiFeature)
        throw std::invalid_argument("Essentia reference method");
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    examples::Builder b({44100,2048,512,512});finite_limits(b,options);
    const char* id=multi?"essentia-finite-multifeature":"essentia-finite-degara";b.graph.graph_id=id;
    finite_source(b,options.maximum_native_pcm_samples);processing(b,multi,true);
    NumericExampleGraph result{id,multi?"Essentia: complete MultiFeature input":"Essentia: complete Degara input",b.finish()};
    describe(result);
    std::erase_if(result.node_names,[](const auto& name){return name.node==100 || name.node==101;});
    for(auto& name:result.node_names)if(name.node==3)name.label="Essentia complete finite input";
    return result;
}
