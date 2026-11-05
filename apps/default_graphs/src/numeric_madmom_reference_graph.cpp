// Recipe of CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Source algorithms remain in shared kernels; this file only composes stages.
#include <core_app/numeric_madmom_reference_graph.h>
#include "example_builder.h"
#include <dsp_kernels/log_frequency_filter_bank.h>
#include <dsp_kernels/numerical_precision.h>
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
using namespace domain;
using namespace signal_contracts;
constexpr std::size_t fft_size=2048,hop=240,frames=2000;
constexpr std::uint32_t analysis_rate=48000;
NumericArray array(std::span<const float> values) {return {std::vector<float>(values.begin(),values.end())};}

OwnedProcessingContract sequence(examples::Builder& b,NodeId from,std::size_t count,
                                 DType type=DType::Float32,std::size_t columns=1) {
    auto c=std::get<NumericContract>(b.output(from).view());
    c.dtype=type;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;
    c.quantity=Quantity::Unspecified;c.unit=NumericUnit::Values;c.clock={};c.coordinates={};
    c.representation={};c.representation.alignment=type==DType::UInt64?alignof(std::uint64_t):alignof(float);
    c.shape={columns==1?Rank::Vector:Rank::Matrix,{Axis::Declared,columns==1?Axis::None:Axis::Component},
             {ExtentBounds{0,count},ExtentBounds{columns,columns}}};
    OwnedProcessingContract result;
    if(own_processing_contract(c,b.limits.frame,b.limits.max_owned_metadata_bytes,result)!=Status::ok)
        throw std::runtime_error("madmom output exceeds profile");
    return result;
}
void output(examples::Builder& b,NodeId id,PortId port,std::size_t count,
            DType type=DType::Float32,std::size_t columns=1) {
    b.node(id).outputs.push_back({port,PortDirection::Output,sequence(b,31,count,type,columns)});
}
void bounded_pcm_ingress(examples::Builder& b) {
    auto c=std::get<NumericContract>(b.output(1).view());c.shape.extents[0]={1,8192};
    OwnedProcessingContract bounded;
    if(own_processing_contract(c,b.limits.frame,b.limits.max_owned_metadata_bytes,bounded)!=Status::ok)
        throw std::invalid_argument("madmom finite PCM ingress");
    b.node(1).outputs[0].contract=std::move(bounded);
}
void source(examples::Builder& b,std::uint32_t rate,bool finite=false,bool reference=false) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},
        {"output_count",NumericLiteral{std::uint64_t{1}}}}});
    if(finite)bounded_pcm_ingress(b);
    b.chain(5,"PcmResampler",1,examples::transform({{"input_rate",NumericLiteral{std::uint64_t(rate)}},
        {"output_rate",NumericLiteral{std::uint64_t(analysis_rate)}},{"mode",std::string{"existing_fir"}}}));
    b.connect(1,5,sample_rate_output,input_sample_rate_input);
    b.chain(20,"FrameBuffer",5,examples::transform({{"size",NumericLiteral{std::uint64_t(fft_size)}},
        {"hop_samples",NumericLiteral{std::uint64_t(hop)}}}));
    b.chain(21,"ArrayLength",20);
    auto window=examples::transform({{"size",NumericLiteral{std::uint64_t(fft_size)}},
        {"function",std::string{"hann"}},{"periodic",false}});
    if(reference)window.values.push_back({"precision",NumericLiteral{std::uint64_t{1}}});
    b.chain(22,"WindowFunction",21,window);
    b.add(23,"ArrayMultiply");b.connect(20,23);b.connect(22,23,0,1);b.derive(23);
}
void spectrum(examples::Builder& b,bool reference=false) {
    b.chain(24,"RealFourier",23,examples::transform({{"fft_size",NumericLiteral{std::uint64_t(fft_size)}}}));
    b.connect(21,24,0,window_size_input);
    auto magnitude=examples::transform({{"fft_size",NumericLiteral{std::uint64_t(fft_size)}},
        {"first_bin",NumericLiteral{std::uint64_t{1}}},{"sample_rate",NumericLiteral{std::uint64_t(analysis_rate)}},
        {"normalize",false},{"representation",std::string{"magnitude"}},{"real_endpoints",true}});
    if(reference)magnitude.values.push_back({"magnitude_norm",std::string{"scaled_ratio_fma"}});
    b.add(25,"AmplitudePhase",magnitude);
    b.connect(24,25);b.connect(24,25,1,1);b.derive(25);
    b.connect(21,25,0,window_size_input);b.connect(5,25,sample_rate_output,target_sample_rate_input);
    if(!reference)b.connect(22,25,0,window_coefficients_input);
}
dsp_kernels::LogFrequencyFilterBank filterbank(bool reference=false) {
    std::array<float,fft_size/2> hz{};
    for(std::size_t k=0;k<hz.size();++k)hz[k]=float(k*analysis_rate)/float(fft_size);
    dsp_kernels::LogFrequencyFilterBank bank;
    dsp_kernels::LogFrequencyFilterConfig config{24,30,17000,440,true,true};
    if(reference)config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    if(!bank.prepare(hz,config))throw std::runtime_error("madmom logarithmic bank");
    return bank;
}
NumericArray positive_weights(const dsp_kernels::LogFrequencyFilterBank& bank,std::size_t first,std::size_t count) {
    NumericArray weights;weights.values.resize(count*(fft_size/2));
    const auto width=fft_size/2;
    for(std::size_t row=0;row<count;++row){
        const auto original=bank.weights().subspan((first+row)*width,width);
        if(original[0]!=0)throw std::runtime_error("madmom recipe requires zero DC filter weight");
        std::copy(original.begin()+1,original.end(),weights.values.begin()+row*width);
        weights.values[(row+1)*width-1]=0; // Extra RealFourier Nyquist is outside source's bank.
    }
    return weights;
}
void join_band_groups(examples::Builder& b,std::size_t groups) {
    NodeId previous=60;
    for(std::size_t group=1;group<groups;++group){
        const auto id=group+1==groups?NodeId{26}:NodeId(70+group);
        b.add(id,"ArrayConcat");b.connect(previous,id);b.connect(NodeId(60+group),id,0,1);b.derive(id);previous=id;
    }
}
void project_bands(examples::Builder& b,const dsp_kernels::LogFrequencyFilterBank& bank,bool reference=false) {
    constexpr std::size_t group_size=32;
    const auto groups=(bank.bands()+group_size-1)/group_size;
    if(groups<2 || groups>8)throw std::runtime_error("madmom filterbank group count");
    for(std::size_t group=0;group<groups;++group){
        const auto first=group*group_size,count=std::min(group_size,bank.bands()-first);
        auto parameters=examples::transform({
            {"weights",positive_weights(bank,first,count)},
            {"centers_hz",array(bank.centers_hz().subspan(first,count))},
            {"compensated",false},{"square_input",false}});
        if(reference)parameters.values.push_back({"accumulation",std::string{"sequential_fma"}});
        b.chain(NodeId(60+group),"FilterMatrixProjection",25,parameters);
    }
    join_band_groups(b,groups);
}
std::size_t difference_lag() {
    std::array<float,fft_size> window{};std::size_t lag=0;
    for(std::size_t i=0;i<window.size();++i)window[i]=dsp_kernels::window_coefficient(i,fft_size,WindowFunction::Hann,false);
    if(!dsp_kernels::spectral_difference_lag(window,float(hop),.5f,lag))throw std::runtime_error("madmom difference lag");
    return lag;
}
void streaming_novelty(examples::Builder& b,bool reference=false) {
    const auto bank=filterbank(reference);
    project_bands(b,bank,reference);
    b.chain(29,"LogCompression",26,examples::transform({{"multiply",NumericLiteral{1.f}},
        {"add",NumericLiteral{1.f}},{"base",NumericLiteral{std::uint64_t{0}}}}));
    b.chain(30,"OnsetStrength",29,examples::transform({{"lag",NumericLiteral{std::uint64_t(difference_lag())}},
        {"max_size",NumericLiteral{std::uint64_t{3}}},{"center",false},{"detrend",false},{"aggregate",std::string{"none"}}}));
    b.chain(3,"FeatureAggregate",30,examples::transform({{"method",NumericLiteral{std::uint64_t{3}}}}));
}

void novelty(examples::Builder& b) {
    streaming_novelty(b);
    b.chain(27,"NumberAccumulator",3,HistoryProcessingParameters{frames,true});
    b.chain(31,"FrameStride",27,examples::transform({{"every_n",NumericLiteral{std::uint64_t{40}}},{"materialize",true}}));
}
std::size_t histogram_bins(MadmomReferenceMode mode) {return mode==MadmomReferenceMode::Crf?551:253;}
TransformProcessingParameters tempo_parameters(MadmomReferenceMode mode) {
    const bool crf=mode==MadmomReferenceMode::Crf;
    auto parameters=examples::transform({{"method",NumericLiteral{std::uint64_t{1}}},{"frame_rate",NumericLiteral{200.f}},
        {"minimum_bpm",NumericLiteral{crf?20.f:40.f}},{"maximum_bpm",NumericLiteral{crf?240.f:250.f}},
        {"history_seconds",NumericLiteral{10.f}},{"alpha",NumericLiteral{.79f}},
        {"activation_smoothing_seconds",NumericLiteral{crf?.09f:.14f}},
        {"histogram_smoothing",NumericLiteral{std::uint64_t(crf?7:9)}}});
    if(crf)parameters.values.push_back({"precision",NumericLiteral{std::uint64_t{1}}});
    return parameters;
}
void tempo(examples::Builder& b,MadmomReferenceMode mode) {
    auto parameters=tempo_parameters(mode);parameters.values.push_back({"interpolate",false});
    b.add(35,"TempoAnalysis",parameters);b.node(35).inputs.clear();b.node(35).outputs.clear();
    const auto bins=histogram_bins(mode);
    output(b,35,0,bins,DType::Float32,2);
    output(b,35,1,bins);output(b,35,2,bins);output(b,35,3,bins);b.connect(31,35);
}
void tracking_node(examples::Builder& b,MadmomReferenceMode mode) {
    auto parameters=tempo_parameters(mode);
    if(mode==MadmomReferenceMode::Crf){
        parameters.values.push_back({"interpolate",false});
        parameters.values.push_back({"interval_sigma",NumericLiteral{.18f}});
        parameters.values.push_back({"num_candidates",NumericLiteral{std::uint64_t{5}}});
        b.add(4,"CrfBeatSequence",parameters);
    }else{
        parameters.values.push_back({"variable_tempo",mode==MadmomReferenceMode::Local});
        parameters.values.push_back({"look_aside",NumericLiteral{.2f}});
        parameters.values.push_back({"look_ahead_seconds",NumericLiteral{10.f}});
        b.add(4,"BeatTrackingSequence",parameters);
    }
}
void tracking(examples::Builder& b,MadmomReferenceMode mode,std::size_t maximum_frames=frames) {
    tracking_node(b,mode);b.node(4).inputs.clear();b.node(4).outputs.clear();
    const auto events=mode==MadmomReferenceMode::Crf?std::max<std::size_t>(1,maximum_frames/50):maximum_frames+1;
    output(b,4,0,events);output(b,4,1,events,DType::UInt64);
    if(mode==MadmomReferenceMode::Crf){output(b,4,2,5,DType::UInt64);output(b,4,3,5);output(b,4,4,histogram_bins(mode),DType::Float32,2);}
    else output(b,4,2,histogram_bins(mode));
    b.connect(31,4);
}
void peaks(examples::Builder& b) {
    b.chain(32,"Spectrogram",3,HistoryProcessingParameters{frames,true});
    b.chain(33,"FrameStride",32,examples::transform({{"every_n",NumericLiteral{std::uint64_t{40}}},{"materialize",true}}));
    b.add(36,"ActivationPeakMatrix",examples::transform({{"frame_rate",NumericLiteral{200.f}},
        {"threshold",NumericLiteral{.5f}},{"pre_average",NumericLiteral{std::uint64_t{0}}},
        {"post_average",NumericLiteral{std::uint64_t{0}}},{"pre_maximum",NumericLiteral{std::uint64_t{1}}},
        {"post_maximum",NumericLiteral{std::uint64_t{1}}}}));
    b.node(36).inputs.clear();b.node(36).outputs.clear();
    output(b,36,0,frames,DType::UInt64);output(b,36,1,frames,DType::UInt64);b.connect(33,36);
}
void views(examples::Builder& b) {
    b.chain(40,"VectorLineView",20);b.chain(44,"VectorLineView",25);b.chain(45,"VectorLineView",26);
    b.chain(46,"VectorLineView",29);b.chain(10,"VectorLineView",27);b.chain(11,"VectorPointView",4);
    b.chain(47,"VectorLineView",35,{},3);b.chain(48,"MatrixImageView",35);
    b.chain(49,"VectorPointView",36);
    b.chain(50,"IndexExtract",35,examples::transform({{"index",NumericLiteral{std::uint64_t{0}}},
        {"column",NumericLiteral{std::uint64_t{0}}}}));
    b.chain(13,"ScalarMetricsView",50);b.chain(51,"VectorLineView",35,{},2);
}
const char* identity(MadmomReferenceMode mode) {
    if(mode==MadmomReferenceMode::Fixed)return "madmom-superflux-fixed";
    return mode==MadmomReferenceMode::Local?"madmom-superflux-local":"madmom-superflux-crf";
}
const char* title(MadmomReferenceMode mode) {
    if(mode==MadmomReferenceMode::Fixed)return "Rhythm: madmom SuperFlux / fixed beats";
    return mode==MadmomReferenceMode::Local?"Rhythm: madmom SuperFlux / local beats":"Rhythm: madmom SuperFlux / CRF beats";
}
void describe(NumericExampleGraph& graph) {
    graph.node_names={{40,"madmom: complete PCM frame"},{44,"madmom: raw spectral magnitude"},
        {45,"madmom: logarithmic frequency bands"},{46,"madmom: log10(1 + magnitude)"},
        {10,"madmom: SuperFlux snapshot"},{11,"madmom: beat seconds within snapshot"},
        {47,"madmom: smoothed interval histogram"},{48,"madmom: BPM and candidate strength"},
        {49,"madmom: onset frame indices within snapshot"},{13,"Tempo, BPM"},
        {51,"madmom: histogram frame intervals"}};
    graph.scalar_names={{13,{"Tempo, BPM"}}};
    graph.positions={{1,0,0},{5,400,0},{20,800,0},{21,800,500},{22,1200,500},{23,1600,0},
        {24,2000,0},{25,2400,0},{26,2900,0},{29,3400,0},{30,3900,0},{3,4350,0},
        {27,4800,0},{31,5300,0},{35,5750,0},{4,6250,0},{32,4800,650},{33,5300,650},{36,5750,650},
        {40,850,-450},{44,2450,-450},{45,2950,-450},{46,3450,-450},{10,4850,-450},
        {11,6750,0},{47,6200,-500},{48,5750,-500},{49,6250,650},
        {50,5750,-1000},{13,6200,-1000},{51,6700,-500}};
    for(const auto& node:graph.graph.nodes){
        if(node.id>=60 && node.id<68)graph.positions.push_back({node.id,2800.,700.+double(node.id-60)*350.});
        if(node.id>=71 && node.id<78)graph.positions.push_back({node.id,3300.+double(node.id-71)*300.,700.});
    }
}
const char* whole_identity(MadmomReferenceMode mode) {
    if(mode==MadmomReferenceMode::Fixed)return "madmom-whole-superflux-fixed";
    return mode==MadmomReferenceMode::Local?"madmom-whole-superflux-local":"madmom-whole-superflux-crf";
}
std::size_t whole_frames(std::size_t samples,std::uint32_t rate,MadmomReferenceMode mode) {
    if(rate!=analysis_rate || samples<fft_size)throw std::invalid_argument("madmom whole native input");
    if(mode!=MadmomReferenceMode::Fixed && mode!=MadmomReferenceMode::Local && mode!=MadmomReferenceMode::Crf)
        throw std::invalid_argument("madmom whole reference mode");
    return 1+(samples-fft_size)/hop;
}
OwnedProcessingContract whole_odf_contract(examples::Builder& b,std::size_t count) {
    auto c=std::get<NumericContract>(b.output(3).view());
    c.dtype=DType::Float32;c.semantic=Semantic::Numeric;c.unit=NumericUnit::Values;
    c.shape={Rank::Vector,{Axis::Time,Axis::None},{{{count,count},{1,1}}}};
    c.clock={"madmom-whole-odf",{1,200},200};
    c.coordinates={};c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,200}};
    c.representation={};c.representation.alignment=alignof(float);
    OwnedProcessingContract result;
    if(own_processing_contract(c,b.limits.frame,b.limits.max_owned_metadata_bytes,result)!=Status::ok)
        throw std::invalid_argument("madmom whole ODF exceeds profile");
    return result;
}
void whole_odf_source(examples::Builder& b,std::size_t count) {
    auto contract=whole_odf_contract(b,count);
    b.add(31,"NetworkInput",EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{200}}}}});
    auto& node=b.node(31);node.inputs.clear();node.outputs.clear();
    node.outputs.push_back({0,PortDirection::Output,std::move(contract)});
}
void whole_views(examples::Builder& b) {
    b.chain(10,"VectorLineView",31);b.chain(11,"VectorPointView",4);
    b.chain(48,"MatrixImageView",35);
}
}
NumericExampleGraph numeric_madmom_reference_example(std::uint32_t source_rate,MadmomReferenceMode mode) {
    if(source_rate<8000 || source_rate>96000)throw std::invalid_argument("madmom native source Fs");
    if(mode!=MadmomReferenceMode::Fixed && mode!=MadmomReferenceMode::Local && mode!=MadmomReferenceMode::Crf)
        throw std::invalid_argument("madmom reference mode");
    examples::Builder b({source_rate,fft_size,512,400});b.graph.graph_id=identity(mode);
    source(b,source_rate);spectrum(b);novelty(b);tempo(b,mode);tracking(b,mode);peaks(b);views(b);
    NumericExampleGraph graph{identity(mode),title(mode),b.finish()};describe(graph);return graph;
}
NumericExampleGraph numeric_madmom_whole_reference_example(std::size_t pcm_samples,MadmomReferenceMode mode,std::uint32_t source_rate) {
    const auto count=whole_frames(pcm_samples,source_rate,mode);
    const bool reference=mode==MadmomReferenceMode::Crf;
    examples::Builder b({source_rate,fft_size,512,400});b.graph.graph_id=whole_identity(mode);
    source(b,source_rate,true,reference);spectrum(b,reference);streaming_novelty(b,reference);whole_odf_source(b,count);
    tempo(b,mode);tracking(b,mode,count);whole_views(b);
    return {whole_identity(mode),title(mode),b.finish()};
}
