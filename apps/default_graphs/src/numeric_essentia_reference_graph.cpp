// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_essentia_reference_graph.h>
#include "example_builder.h"
#include "essentia_graph_parts.h"
#include <rhythm_graph_adapters/essentia_ports.h>
#include <domain/numeric/array_processing_ports.h>
#include <rhythm_graph_adapters/essentia_composition.h>
#include <rhythm_graph_adapters/tracking_composition.h>
#include <rhythm_graph_adapters/sequence_source_composition.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/temporal_window.h>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace {
using namespace domain;using namespace signal_contracts;using namespace rhythm_graph_adapters;
template<class T> constexpr PortId port(T value) noexcept {return static_cast<PortId>(value);}
struct Link {NodeId source;PortId output=port(ArrayOutput::Values),input=port(ArrayInput::Values);};
using Maker=Status (*)(NodeId,std::string_view,std::span<const OwnedProcessingContract>,
    TransformProcessingParameters,const FrameLimits&,std::size_t,ConfiguredProcessingNode&);
void stage(examples::Builder& b,NodeId id,std::string_view kind,Maker make,
    TransformProcessingParameters parameters,std::initializer_list<Link> links) {
    std::vector<OwnedProcessingContract> inputs;
    for(const auto& link:links)inputs.push_back(b.output(link.source,link.output));
    ConfiguredProcessingNode node;
    const auto status=make(id,kind,inputs,std::move(parameters),b.limits.frame,b.limits.max_owned_metadata_bytes,node);
    if(status!=Status::ok)throw std::runtime_error("Essentia stage: "+std::string(kind)+" id="+std::to_string(id)+" status="+std::to_string(unsigned(status)));
    b.graph.nodes.push_back(std::move(node));
    for(const auto& link:links)b.connect(link.source,id,link.output,link.input);
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
        {"backend_id",std::string{"kiss_fft130_float32"}}}));
}
void polar(examples::Builder& b,NodeId first) {
    b.add(first+6,"ComplexPolar",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{2048}}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"positive_half",true},{"real_endpoints",false},
        {"magnitude_norm",std::string{"squared_norm"}},{"zero_phase",std::string{"atan2"}}}));
    b.connect(first+3,first+6);b.connect(first+3,first+6,port(FourierOutput::Imaginary),port(FourierInput::Imaginary));b.derive(first+6);
}
void spectrum(examples::Builder& b,NodeId first,std::uint64_t hop) {
    b.chain(first,"TemporalFrameSequence",b.graph.graph_id.starts_with("essentia-finite")?5:3,examples::transform({{"window_length",NumericLiteral{std::uint64_t{2048}}},
        {"hop",NumericLiteral{hop}},{"center",false},{"tail",NumericLiteral{std::uint64_t{1}}}}));
    if(first==10){
        b.chain(18,"ArrayLength",10,examples::transform({{"axis",NumericLiteral{std::uint64_t{1}}}}));
        b.connect(18,4);b.derive(4);
    }
    b.add(first+1,"ArrayMultiply");b.connect(first,first+1);b.connect(4,first+1,0,1);b.derive(first+1);
    b.chain(first+2,"IndexExtract",first+1,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",indices(2048,1024)}}));
    fourier(b,first);polar(b,first);
}
void odf(examples::Builder& b,bool multi,std::size_t frames) {
    using namespace rhythm_graph_adapters;
    stage(b,40,"EssentiaOnsetRows",make_essentia_operation_node,
        examples::transform({{"method",NumericLiteral{std::uint64_t{1}}},{"frame_rate",NumericLiteral{44100.f/1024.f}}}),{{16,port(PolarOutput::Magnitude),port(SpectralRowsInput::Magnitude)},{16,port(PolarOutput::Phase),port(SpectralRowsInput::Phase)}});
    if(!multi)return;
    stage(b,41,"EssentiaOnsetRows",make_essentia_operation_node,
        examples::transform({{"method",NumericLiteral{std::uint64_t{5}}},{"frame_rate",NumericLiteral{44100.f/1024.f}}}),{{16}});
    const auto mel=essentia_graph::mel_input(b,16);
    stage(b,42,"EssentiaOnsetRows",make_essentia_operation_node,
        examples::transform({{"method",NumericLiteral{std::uint64_t{4}}},{"frame_rate",NumericLiteral{44100.f/1024.f}}}),{{mel}});
    stage(b,43,"EssentiaNoveltyRows",make_essentia_operation_node,{},{{26,port(PolarOutput::Magnitude),port(SpectralRowsInput::Magnitude)},{26,port(PolarOutput::Phase),port(SpectralRowsInput::Phase)}});
    stage(b,44,"EssentiaErbRows",make_essentia_operation_node,{},{{43}});
    essentia_graph::band_emphasis(b,44,frames);
    stage(b,46,"EssentiaInformationRows",make_essentia_operation_node,
        examples::transform({{"frame_rate",NumericLiteral{44100.f/512.f}}}),{{26}});
}
void candidates(examples::Builder& b,bool multi) {
    if(multi)stage(b,47,"EssentiaCandidateInputs",rhythm_graph_adapters::make_essentia_operation_node,
        {},{{40,port(OnsetRowsOutput::Curve),port(CandidateCurve::Complex)},
            {41,port(OnsetRowsOutput::Curve),port(CandidateCurve::Rms)},{42,port(OnsetRowsOutput::Curve),port(CandidateCurve::MelFlux)},
            {45,port(ArrayOutput::Values),port(CandidateCurve::Emphasis)},{46,port(OnsetRowsOutput::Curve),port(CandidateCurve::Information)}});
    constexpr std::array curves{CandidateCurve::Complex,CandidateCurve::Rms,CandidateCurve::MelFlux,CandidateCurve::Emphasis,CandidateCurve::Information};
    for(std::size_t i=0;i<(multi?5u:1u);++i)
        essentia_graph::candidate(b,multi?47:40,multi?port(curves[i]):port(OnsetRowsOutput::Curve),NodeId(50+i),44100.f/(i<3?1024.f:512.f),i<3?2:1);
}
void degara_summary(examples::Builder& b,NodeId id) {
    b.add(59,"Number",EndpointProcessingParameters{{{"value",NumericLiteral{0.f}}}});
    stage(b,id,"BeatIntervalSummary",rhythm_graph_adapters::make_sequence_source_node,{},{{50,port(RenewalOutput::Times),port(SummaryInput::Ticks)},{59,port(ArrayOutput::Values),port(SummaryInput::Confidence)}});
}
void summary(examples::Builder& b,bool multi) {
    using namespace rhythm_graph_adapters;
    if(multi){
        stage(b,60,"EssentiaAgreement",make_essentia_operation_node,
            examples::transform({{"minimum_time",NumericLiteral{5.f}}}),{{50,port(RenewalOutput::Times),port(CandidateCurve::Complex)},
                {51,port(RenewalOutput::Times),port(CandidateCurve::Rms)},{52,port(RenewalOutput::Times),port(CandidateCurve::MelFlux)},
                {53,port(RenewalOutput::Times),port(CandidateCurve::Emphasis)},{54,port(RenewalOutput::Times),port(CandidateCurve::Information)}});
        stage(b,61,"BeatIntervalSummary",make_sequence_source_node,{},{{60,port(AgreementOutput::Ticks),port(SummaryInput::Ticks)},{60,port(AgreementOutput::Confidence),port(SummaryInput::Confidence)}});
    }else degara_summary(b,61);
}
void result_views(examples::Builder& b,bool multi) {
    if(multi)b.chain(114,"VectorLineView",60);
    else b.chain(103,"VectorLineView",50,{},static_cast<PortId>(rhythm_graph_adapters::RenewalOutput::Times));
    b.chain(107,"ScalarMetricsView",61);b.connect(61,107,1,1);
}
void processing(examples::Builder& b,bool multi,std::size_t samples=352800) {
    coefficients(b);spectrum(b,10,1024);if(multi)spectrum(b,20,512);
    dsp_kernels::TemporalWindowConfig window;window.length=2048;window.hop=512;window.center=false;window.tail=dsp_kernels::TemporalTail::CoverEnd;
    std::size_t frames=0;if(!dsp_kernels::PreparedTemporalWindow::frame_count(window,samples,frames))throw std::invalid_argument("Essentia source frames");
    odf(b,multi,frames);candidates(b,multi);summary(b,multi);result_views(b,multi);
}
void compose(examples::Builder& b,bool multi) {
    source(b);processing(b,multi);
}
examples::Builder live_graph(const char* id,bool multi) {
    examples::Builder b({44100,2048,512,512});
    // Explicit optional research batch geometry; main/SDK budgets are unchanged.
    b.limits.frame.max_elements=2097152;b.limits.frame.max_payload_bytes=8388608;
    b.graph.graph_id=id;
    compose(b,multi);
    return b;
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
void describe(NumericExampleGraph& g,bool multi,bool finite=false) {
    g.node_names={{3,finite?"Essentia complete finite input":"Essentia analysis session"},
        {4,"Essentia normalized symmetric Hann"},
        {multi?114:103,multi?"Essentia: MultiFeature beats":"Essentia: Degara beats"},
        {107,multi?"Essentia: MultiFeature BPM and confidence":"Essentia: Degara BPM and confidence"}};
    g.scalar_names={{107,{"Tempo, BPM","Confidence"}}};
    for(std::size_t i=0;i<g.graph.nodes.size();++i)
        g.positions.push_back({g.graph.nodes[i].id,double(i%7)*480,double(i/7)*440});
}
void describe_combined(NumericExampleGraph& g) {
    describe(g,true);
    g.node_names.push_back({103,"Essentia: Degara beats"});
    g.node_names.push_back({108,"Essentia: Degara BPM and confidence"});
    g.scalar_names.push_back({108,{"Tempo, BPM","Confidence"}});
}
}
NumericExampleGraph numeric_essentia_reference_example(EssentiaReferenceMethod method) {
    if(method!=EssentiaReferenceMethod::Degara && method!=EssentiaReferenceMethod::MultiFeature)
        throw std::invalid_argument("Essentia reference method");
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    const char* id=multi?"essentia-reference-multifeature":"essentia-reference-degara";
    auto b=live_graph(id,multi);
    NumericExampleGraph result{id,multi?"Essentia: RhythmExtractor2013 MultiFeature":"Essentia: RhythmExtractor2013 Degara",b.finish()};
    describe(result,multi);return result;
}
NumericExampleGraph numeric_essentia_combined_reference_example() {
    constexpr auto id="essentia-reference";
    auto b=live_graph(id,true);
    degara_summary(b,62);
    b.chain(103,"VectorLineView",50,{},static_cast<PortId>(rhythm_graph_adapters::RenewalOutput::Times));
    b.chain(108,"ScalarMetricsView",62);
    b.connect(62,108,1,1);
    NumericExampleGraph result{id,"Essentia: Degara and MultiFeature",b.finish()};
    describe_combined(result);
    return result;
}
NumericExampleGraph numeric_essentia_finite_reference_example(EssentiaFiniteOptions options,EssentiaReferenceMethod method) {
    if(method!=EssentiaReferenceMethod::Degara && method!=EssentiaReferenceMethod::MultiFeature)
        throw std::invalid_argument("Essentia reference method");
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    examples::Builder b({44100,2048,512,512});finite_limits(b,options);
    const char* id=multi?"essentia-finite-multifeature":"essentia-finite-degara";b.graph.graph_id=id;
    finite_source(b,options.maximum_native_pcm_samples);
    b.chain(5,"IndexExtract",3,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",NumericArray{}},{"length",NumericLiteral{std::uint64_t(options.input_samples)}}}));
    processing(b,multi,options.input_samples);
    NumericExampleGraph result{id,multi?"Essentia: complete MultiFeature input":"Essentia: complete Degara input",b.finish()};
    describe(result,multi,true);
    return result;
}
