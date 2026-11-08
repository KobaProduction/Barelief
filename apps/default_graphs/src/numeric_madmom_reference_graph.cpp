// Recipe of CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Source algorithms remain in shared kernels; this file only composes stages.
#include <core_app/numeric_madmom_reference_graph.h>
#include "example_builder.h"
#include <domain/numeric/atomic_operation_ports.h>
#include <dsp_kernels/log_frequency_filter_bank.h>
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/window_math.h>
#include <dsp_kernels/axis_math.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
using namespace domain;
using namespace signal_contracts;
enum class MadmomReferenceMode {Fixed,Local,Crf};
constexpr std::size_t fft_size=2048,hop=240,frames=2000;
constexpr std::uint32_t analysis_rate=48000;
namespace beat_ports=domain::atomic_ports::decoder;
namespace spectrum_ports {constexpr PortId real=0,imaginary=1,magnitude=0;}
namespace product_ports {constexpr PortId samples=0,coefficients=1;}
namespace concat_ports {constexpr PortId first=0,next=1;}
NumericArray array(std::span<const float> values) {return {std::vector<float>(values.begin(),values.end())};}

OwnedProcessingContract sequence(examples::Builder& b,NodeId from,std::size_t count,
                                 DType type=DType::Float32,std::size_t columns=1) {
    auto c=std::get<NumericContract>(b.output(from).view());
    c.dtype=type;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;
    c.quantity=Quantity::Unspecified;c.unit=NumericUnit::Values;c.clock={};c.coordinates={};
    c.representation={};c.representation.alignment=type==DType::UInt64 || type==DType::Float64?alignof(std::uint64_t):alignof(float);
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
void source(examples::Builder& b,std::uint32_t rate) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},
        {"output_count",NumericLiteral{std::uint64_t{1}}}}});
    b.chain(5,"PcmResampler",1,examples::transform({{"input_rate",NumericLiteral{std::uint64_t(rate)}},
        {"output_rate",NumericLiteral{std::uint64_t(analysis_rate)}},{"mode",std::string{"existing_fir"}}}));
    b.connect(1,5,sample_rate_output,input_sample_rate_input);
    b.chain(20,"FrameBuffer",5,examples::transform({{"size",NumericLiteral{std::uint64_t(fft_size)}},
        {"hop_samples",NumericLiteral{std::uint64_t(hop)}}}));
    b.chain(21,"ArrayLength",20);
    auto window=examples::transform({{"size",NumericLiteral{std::uint64_t(fft_size)}},
        {"function",std::string{"hann"}},{"periodic",false}});
    b.chain(22,"WindowFunction",21,window);
    b.add(23,"ArrayMultiply");
    b.connect(20,23,processing_output,product_ports::samples);
    b.connect(22,23,processing_output,product_ports::coefficients);b.derive(23);
}
void spectrum(examples::Builder& b) {
    b.chain(24,"RealFourier",23,examples::transform({{"fft_size",NumericLiteral{std::uint64_t(fft_size)}}}));
    b.connect(21,24,0,window_size_input);
    auto magnitude=examples::transform({{"fft_size",NumericLiteral{std::uint64_t(fft_size)}},
        {"sample_rate",NumericLiteral{std::uint64_t(analysis_rate)}},
        {"normalize",false},{"representation",std::string{"magnitude"}},{"real_endpoints",true}});
    b.add(25,"AmplitudePhase",magnitude);
    b.connect(24,25,spectrum_ports::real,spectrum_ports::real);
    b.connect(24,25,spectrum_ports::imaginary,spectrum_ports::imaginary);b.derive(25);
    b.connect(21,25,0,window_size_input);b.connect(5,25,sample_rate_output,target_sample_rate_input);
    b.connect(22,25,0,window_coefficients_input);
}
dsp_kernels::LogFrequencyFilterBank filterbank() {
    std::array<float,fft_size/2> hz{};
    for(std::size_t k=0;k<hz.size();++k)hz[k]=float(k*analysis_rate)/float(fft_size);
    dsp_kernels::LogFrequencyFilterBank bank;
    dsp_kernels::LogFrequencyFilterConfig config{24,30,17000,440,true,true};
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
        weights.values[(row+1)*width-1]=0; // Nyquist is outside the source filterbank.
    }
    return weights;
}
void join_band_groups(examples::Builder& b,std::size_t groups) {
    NodeId previous=60;
    for(std::size_t group=1;group<groups;++group){
        const auto id=group+1==groups?NodeId{26}:NodeId(70+group);
        b.add(id,"ArrayConcat");b.connect(previous,id,processing_output,concat_ports::first);
        b.connect(NodeId(60+group),id,processing_output,concat_ports::next);b.derive(id);previous=id;
    }
}
void project_bands(examples::Builder& b,const dsp_kernels::LogFrequencyFilterBank& bank) {
    constexpr std::size_t group_size=32;
    const auto groups=(bank.bands()+group_size-1)/group_size;
    if(groups<2 || groups>8)throw std::runtime_error("madmom filterbank group count");
    for(std::size_t group=0;group<groups;++group){
        const auto first=group*group_size,count=std::min(group_size,bank.bands()-first);
        auto parameters=examples::transform({
            {"weights",positive_weights(bank,first,count)},
            {"centers_hz",array(bank.centers_hz().subspan(first,count))},
            {"compensated",false},{"square_input",false}});
        b.chain(NodeId(60+group),"FilterMatrixProjection",25,parameters,spectrum_ports::magnitude);
    }
    join_band_groups(b,groups);
}
std::size_t difference_lag() {
    std::array<float,fft_size> window{};std::size_t lag=0;
    for(std::size_t i=0;i<window.size();++i)window[i]=dsp_kernels::window_coefficient(i,fft_size,WindowFunction::Hann,false);
    if(!dsp_kernels::spectral_difference_lag(window,float(hop),.5f,lag))throw std::runtime_error("madmom difference lag");
    return lag;
}
void streaming_novelty(examples::Builder& b) {
    const auto bank=filterbank();
    project_bands(b,bank);
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
namespace maximum_ports=domain::atomic_ports::maximum;
namespace bank_ports=domain::atomic_ports::comb;
namespace gather_ports=domain::atomic_ports::gather;
namespace convolution_ports=domain::atomic_ports::convolution;
void hamming(examples::Builder& b,NodeId size_id,NodeId window_id,std::size_t length,bool reference=false) {
    b.add(size_id,"UInt32Constant",EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t(length)}}}});
    b.chain(window_id,"WindowFunction",size_id,examples::transform({{"size",NumericLiteral{std::uint64_t(length)}},
        {"function",std::string{"hamming"}},{"periodic",false},{"precision",NumericLiteral{std::uint64_t(reference)}},
        {"evaluation",NumericLiteral{std::uint64_t{2}}}}));
}
void convolution(examples::Builder& b,NodeId id,NodeId values,NodeId kernel,std::size_t axis=0) {
    b.add(id,"SameConvolution",examples::transform({{"axis",NumericLiteral{std::uint64_t(axis)}}}));
    b.connect(values,id,processing_output,convolution_ports::values);
    b.connect(kernel,id,processing_output,convolution_ports::kernel);b.derive(id);
}
void gather(examples::Builder& b,NodeId id,NodeId table,PortId table_port,NodeId indices) {
    b.add(id,"ArrayGather");b.connect(table,id,table_port,gather_ports::table);
    b.connect(indices,id,maximum_ports::indices,gather_ports::indices);b.derive(id);
}
void maximum(examples::Builder& b,NodeId id,NodeId values,std::size_t axis) {
    b.chain(id,"AxisMaximum",values,examples::transform({{"axis",NumericLiteral{std::uint64_t(axis)}}}));
}
NumericArray interval_axis(std::size_t low,std::size_t high) {
    NumericArray result;result.values.resize(high-low+1);
    for(std::size_t i=0;i<result.values.size();++i)result.values[i]=float(i+low);
    return result;
}
void comb_histogram(examples::Builder& b,NodeId bank,NodeId maxima,NodeId sum,NodeId input,bool crf) {
    b.chain(bank,"CombFilterBank",input,examples::transform({{"delays",interval_axis(crf?50:48,crf?600:300)},
        {"alpha",NumericArray{{.79f}}},{"direction",NumericLiteral{std::uint64_t{1}}},{"reset_per_call",true}}));
    maximum(b,maxima,bank,1);
    b.chain(sum,"AxisSum",maxima,examples::transform({{"axis",NumericLiteral{std::uint64_t{0}}},{"cumulative",false},
        {"accumulation",NumericLiteral{std::uint64_t(dsp_kernels::AxisSumAccumulation::Pairwise)}}}),maximum_ports::tied_values);
}
void fixed_intervals(examples::Builder& b) {
    hamming(b,90,91,28);convolution(b,92,31,91);
    comb_histogram(b,93,121,122,92,false);hamming(b,94,95,9);convolution(b,96,122,95);
    maximum(b,97,96,0);gather(b,98,93,bank_ports::delays,97);
}
void local_intervals(examples::Builder& b) {
    // Input length is bounded by the half-context. Each centered 4000-frame window
    // contains all activations, preceded by zeros and followed by a varying
    // zero suffix. Zero prefixes preserve comb state; prefix sums therefore
    // expose exactly the original histogram for every prediction index.
    b.chain(99,"IndexExtract",92,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",NumericArray{}},{"pad_after",NumericLiteral{std::uint64_t(frames-1)}}}));
    b.chain(100,"CombFilterBank",99,examples::transform({{"delays",interval_axis(48,300)},{"alpha",NumericArray{{.79f}}},{"reset_per_call",true}}));
    maximum(b,101,100,1);
    b.chain(102,"AxisSum",101,examples::transform({{"axis",NumericLiteral{std::uint64_t{0}}},{"cumulative",true}}),maximum_ports::tied_values);
    b.chain(103,"IndexExtract",102,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",NumericArray{}},{"axis",NumericLiteral{std::uint64_t{0}}},
        {"first",NumericLiteral{std::uint64_t(frames-1)}},{"length",NumericLiteral{std::uint64_t(frames)}}}));
    convolution(b,104,103,95,1);maximum(b,105,104,1);gather(b,106,100,bank_ports::delays,105);
}
void map(examples::Builder& b,NodeId id,NodeId input,std::size_t dtype,std::size_t operation=0,float scale=1) {
    b.chain(id,"NumericMap",input,examples::transform({{"output_dtype",NumericLiteral{std::uint64_t(dtype)}},
        {"operation",NumericLiteral{std::uint64_t(operation)}},{"scale",NumericLiteral{scale}}}));
}
void crf_intervals(examples::Builder& b) {
    map(b,108,31,1);hamming(b,109,110,18,true);convolution(b,111,108,110);
    comb_histogram(b,112,123,124,111,true);hamming(b,113,114,7,true);convolution(b,115,124,114);
    b.chain(116,"OrderedPeaks",115,examples::transform({{"maximum_count",NumericLiteral{std::uint64_t{5}}}}));
    gather(b,117,112,bank_ports::delays,116);
    // Preserve the source's Double interval -> BPM -> truncated interval route.
    map(b,118,117,1,1,12000);map(b,119,118,2,1,12000);map(b,120,111,0);
}
void tracking(examples::Builder& b,NodeId id,NodeId activations,NodeId intervals,MadmomReferenceMode mode) {
    const bool crf=mode==MadmomReferenceMode::Crf;
    b.add(id,crf?"CrfBeatSequence":"BeatTrackingSequence",examples::transform({
        {"frame_rate",NumericLiteral{200.f}},{"minimum_interval",NumericLiteral{std::uint64_t(crf?50:48)}},
        {"maximum_interval",NumericLiteral{std::uint64_t(crf?600:300)}},
        {"variable_tempo",mode==MadmomReferenceMode::Local},{"look_ahead_frames",NumericLiteral{std::uint64_t(frames)}},
        {"look_aside",NumericLiteral{.2f}},{"interval_sigma",NumericLiteral{.18f}},{"num_candidates",NumericLiteral{std::uint64_t{5}}}}));
    b.node(id).inputs.clear();b.node(id).outputs.clear();
    const auto events=crf?frames/50:frames+1;
    output(b,id,beat_ports::times,events);output(b,id,beat_ports::positions,events,DType::UInt64);
    if(crf){output(b,id,beat_ports::trials,5,DType::UInt64);output(b,id,beat_ports::scores,5);}
    b.connect(activations,id,processing_output,beat_ports::activations);b.connect(intervals,id,processing_output,beat_ports::intervals);
}
void position_filter_groups(NumericExampleGraph& graph) {
    for(const auto& node:graph.graph.nodes){
        if(node.id>=60 && node.id<68)graph.positions.push_back({node.id,2800.,700.+double(node.id-60)*350.});
        if(node.id>=71 && node.id<78)graph.positions.push_back({node.id,3300.+double(node.id-71)*300.,700.});
    }
}
void position_temporal_branches(NumericExampleGraph& graph) {
    const std::array points{
        NumericExampleNodePosition{90,5400,700},NumericExampleNodePosition{91,5800,700},NumericExampleNodePosition{92,6200,0},
        NumericExampleNodePosition{93,6600,0},NumericExampleNodePosition{94,7000,900},NumericExampleNodePosition{95,7400,900},
        NumericExampleNodePosition{96,7800,0},NumericExampleNodePosition{97,8200,0},NumericExampleNodePosition{98,8600,0},
        NumericExampleNodePosition{99,6200,2000},NumericExampleNodePosition{100,6600,2000},NumericExampleNodePosition{101,7000,2000},
        NumericExampleNodePosition{102,7400,2000},NumericExampleNodePosition{103,7800,2000},NumericExampleNodePosition{104,8200,2000},
        NumericExampleNodePosition{105,8600,2000},NumericExampleNodePosition{106,9000,2000},
        NumericExampleNodePosition{108,5800,-1800},NumericExampleNodePosition{109,5400,-1200},NumericExampleNodePosition{110,5800,-1200},
        NumericExampleNodePosition{111,6200,-1800},NumericExampleNodePosition{112,6600,-1800},NumericExampleNodePosition{113,7000,-1000},
        NumericExampleNodePosition{114,7400,-1000},NumericExampleNodePosition{115,7800,-1800},NumericExampleNodePosition{116,8200,-1800},
        NumericExampleNodePosition{117,8600,-1800},NumericExampleNodePosition{118,9000,-1800},NumericExampleNodePosition{119,9400,-1800},
        NumericExampleNodePosition{120,8200,-2300},NumericExampleNodePosition{121,7000,0},
        NumericExampleNodePosition{122,7400,0},NumericExampleNodePosition{123,7000,-1800},NumericExampleNodePosition{124,7400,-1800}};
    graph.positions.insert(graph.positions.end(),points.begin(),points.end());
}
void beat_views(examples::Builder& b) {
    b.chain(11,"VectorPointView",4,{},beat_ports::times);
    b.chain(82,"VectorPointView",80,{},beat_ports::times);
    b.chain(83,"VectorPointView",81,{},beat_ports::times);
}
void describe_combined(NumericExampleGraph& graph) {
    graph.node_names={{11,"madmom: CRF beats"},{82,"madmom: Fixed beats"},{83,"madmom: Local beats"}};
    graph.positions={{1,0,0},{5,400,0},{20,800,0},{21,800,500},{22,1200,500},{23,1600,0},
        {24,2000,0},{25,2400,0},{26,2900,0},{29,3400,0},{30,3900,0},{3,4350,0},
        {27,4800,0},{31,5300,0},{4,9800,-1800},{80,9000,0},{81,9400,2000},
        {11,10200,-1800},{82,9400,0},{83,9800,2000}};
    position_filter_groups(graph);position_temporal_branches(graph);
}
}
NumericExampleGraph numeric_madmom_combined_reference_example(std::uint32_t source_rate) {
    if(source_rate<8000 || source_rate>96000)throw std::invalid_argument("madmom native source Fs");
    constexpr auto id="madmom-superflux-reference";
    examples::Builder b({source_rate,fft_size,512,400});
    b.graph.graph_id=id;
    source(b,source_rate);
    spectrum(b);
    novelty(b);
    fixed_intervals(b);local_intervals(b);crf_intervals(b);
    tracking(b,4,120,119,MadmomReferenceMode::Crf);
    tracking(b,80,92,98,MadmomReferenceMode::Fixed);
    tracking(b,81,92,106,MadmomReferenceMode::Local);
    beat_views(b);
    NumericExampleGraph graph{id,"madmom: Fixed, Local and CRF",b.finish()};
    describe_combined(graph);
    return graph;
}
