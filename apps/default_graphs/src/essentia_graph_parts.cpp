// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_graph_parts.h"
#include <domain/numeric/array_math.h>
#include <domain/numeric/array_processing_ports.h>
#include <dsp_kernels/sequence_statistics.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/vector_normalization.h>
#include <rhythm_graph_adapters/tracking_composition.h>
#include <rhythm_graph_adapters/essentia_ports.h>
#include <rhythm_reference_ports/triangular_spectral_bands.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace essentia_graph {
namespace {
using namespace domain;using namespace signal_contracts;using namespace rhythm_graph_adapters;
template<class T> constexpr PortId port(T value) noexcept {return static_cast<PortId>(value);}
NumericLiteral integer(std::size_t value){return NumericLiteral{std::uint64_t(value)};}
void connect(examples::Builder& b,NodeId source,NodeId target,PortId output,PortId input){b.connect(source,target,output,input);}
void binary(examples::Builder& b,NodeId id,NodeId left,NodeId right,ArrayMathOperation operation=ArrayMathOperation::Multiply,bool late_right=false){
    b.add(id,"ArrayMultiply",examples::transform({{"operation",std::string(array_math_name(operation))},
        {"frame_input",integer(0)},{"trigger_input",NumericLiteral{std::int64_t(late_right || operation==ArrayMathOperation::Add || operation==ArrayMathOperation::Subtract?1:-1)}}}));
    connect(b,left,id,port(ArrayOutput::Values),port(BinaryInput::Left));
    connect(b,right,id,port(ArrayOutput::Values),port(BinaryInput::Right));b.derive(id);
}
void fill(examples::Builder& b,NodeId id,NodeId input,NodeId length,std::size_t axis,std::size_t count,float value,std::span<const float> values={}){
    b.chain(length,"ArrayLength",input,examples::transform({{"axis",integer(axis)}}));
    b.add(id,"ArrayFill",examples::transform({{"size",integer(count)},{"value",NumericLiteral{value}},
        {"values",NumericArray{std::vector<float>(values.begin(),values.end())}}}));
    connect(b,length,id,port(ArrayOutput::Values),port(ArrayInput::Values));b.derive(id);
}
void values(examples::Builder& b,NodeId id,std::span<const float> values,NodeId input,NodeId length,std::size_t axis){
    fill(b,id,input,length,axis,values.size(),0,values);
}
void filter(examples::Builder& b,NodeId id,NodeId input,std::size_t length,std::size_t axis,FilterStatePolicy state,NodeId initial=0){
    const NumericArray numerator{std::vector<float>(length,1.f/float(length))};
    b.chain(id,"RecursiveFilter",input,examples::transform({{"numerator",numerator},{"denominator",NumericArray{{1}}},
        {"axis",integer(axis)},{"state_policy",integer(std::size_t(state))}}));
    if(initial){connect(b,initial,id,port(RecursiveFilterOutput::FinalState),port(RecursiveFilterInput::InitialState));b.derive(id);}
}
void reindex(examples::Builder& b,NodeId id,NodeId input,std::size_t axis,std::size_t first=0,std::size_t length=0,
    std::size_t before=0,std::size_t after=0,ArrayPadding padding=ArrayPadding::Constant,std::size_t total=0){
    b.chain(id,"IndexExtract",input,examples::transform({{"mode",integer(1)},{"indices",NumericArray{}},{"axis",integer(axis)},{"first",integer(first)},{"length",integer(length)},
        {"pad_before",integer(before)},{"pad_after",integer(after)},{"padding",integer(std::size_t(padding))},{"output_length",integer(total)}}));
}
void normalize(examples::Builder& b,NodeId id,NodeId input,dsp_kernels::VectorNormalization method,std::size_t axis){
    b.chain(id,"VectorNormalize",input,examples::transform({{"axis",integer(axis)},{"method",integer(std::size_t(method))},{"minimum_scale",NumericLiteral{0.f}}}));
}
void comb(examples::Builder& b,NodeId id,NodeId correlation,NodeId weights,std::size_t first,std::size_t stop){
    const std::array inputs{b.output(correlation),b.output(weights)};ConfiguredProcessingNode node;
    const auto status=make_tracking_operation_node(id,"HarmonicComb",inputs,examples::transform({{"first",integer(first)},
        {"stop",integer(stop)},{"harmonics",integer(4)}}),b.limits.frame,b.limits.max_owned_metadata_bytes,node);
    if(status!=Status::ok)throw std::runtime_error("Essentia harmonic comb: id="+std::to_string(id)+" status="+std::to_string(unsigned(status)));
    b.graph.nodes.push_back(std::move(node));
    connect(b,correlation,id,port(ArrayOutput::Values),port(HarmonicCombInput::Correlation));
    connect(b,weights,id,port(ArrayOutput::Values),port(HarmonicCombInput::Weights));
}
std::vector<float> rayleigh(std::size_t count,float scale,bool normalized){
    std::vector<float> result(count);
    if(!dsp_kernels::rayleigh_weights(result,scale) || (normalized && !dsp_kernels::normalize_vector(result,dsp_kernels::VectorNormalization::Sum)))
        throw std::runtime_error("Essentia Rayleigh coefficients");
    return result;
}
void band_threshold(examples::Builder& b,NodeId bands){
    normalize(b,300,bands,dsp_kernels::VectorNormalization::StandardDeviation,0);
    reindex(b,301,300,0,0,0,0,16,ArrayPadding::Edge);
    filter(b,302,301,17,0,FilterStatePolicy::ResetRows);reindex(b,303,302,0,16);
    binary(b,304,300,303,ArrayMathOperation::Subtract);b.chain(305,"Clamp",304);
    b.chain(306,"IndexExtract",300,examples::transform({{"mode",integer(1)},{"axis",integer(0)},{"indices",NumericArray{std::vector<float>(16,-1)}}}));
    b.add(307,"ArrayConcat",examples::transform({{"axis",integer(0)}}));
    connect(b,305,307,port(ArrayOutput::Values),port(BinaryInput::Left));
    connect(b,306,307,port(ArrayOutput::Values),port(BinaryInput::Right));b.derive(307);
    b.chain(308,"MatrixTranspose",307);
}
std::size_t fft_length(std::size_t samples){
    std::size_t result=8;while(result<2*samples)result*=2;
    if(result>65536)throw std::invalid_argument("Essentia correlation FFT capacity");return result;
}
NumericArray hermitian_indices(std::size_t fft){
    NumericArray indices;indices.values.resize(fft);
    for(std::size_t i=0;i<fft;++i)indices.values[i]=float(i<=fft/2?i:fft-i);
    return indices;
}
void band_fourier(examples::Builder& b,std::size_t fft){
    reindex(b,309,308,1,0,0,0,0,ArrayPadding::Constant,fft);
    b.chain(310,"RealFourier",309,examples::transform({{"fft_size",integer(fft)}}));
    binary(b,311,310,310,ArrayMathOperation::Multiply,true); // both real operands must be current
    b.add(312,"ArrayMultiply",examples::transform({{"frame_input",integer(0)},{"trigger_input",integer(1)}}));
    connect(b,310,312,port(FourierOutput::Imaginary),port(BinaryInput::Left));connect(b,310,312,port(FourierOutput::Imaginary),port(BinaryInput::Right));b.derive(312);
    binary(b,313,311,312,ArrayMathOperation::Add);
    b.chain(314,"IndexExtract",313,examples::transform({{"mode",integer(1)},{"indices",hermitian_indices(fft)}}));
    fill(b,315,314,332,1,fft,0);binary(b,316,314,315);
    b.add(317,"InverseFourier",examples::transform({{"fft_size",integer(fft)}}));
    connect(b,314,317,port(ArrayOutput::Values),port(FourierInput::Real));
    connect(b,316,317,port(ArrayOutput::Values),port(FourierInput::Imaginary));b.derive(317);
}
void band_comb(examples::Builder& b,std::size_t periods){
    auto weights=rayleigh(periods,std::round(43.f*512.f/float(periods)),false);
    for(std::size_t i=0;i+1<weights.size();++i)weights[i]=weights[i+1];weights.back()=0;
    values(b,323,weights,321,331,1);comb(b,324,322,323,2,(periods-3)/4-2);
}
void band_correlation(examples::Builder& b,std::size_t frames){
    constexpr std::size_t periods=512;
    band_fourier(b,fft_length(frames+16));const auto lags=std::min(periods,frames+16);
    reindex(b,318,317,1,0,lags);
    std::vector<float> counts(lags);for(std::size_t lag=0;lag<lags;++lag)counts[lag]=float(frames+16-lag);
    values(b,319,counts,318,330,1);binary(b,320,318,319,ArrayMathOperation::Divide);
    reindex(b,321,320,1,0,0,0,0,ArrayPadding::Constant,periods);
    reindex(b,322,321,1,1,0,0,1);band_comb(b,periods);
}
void band_selection(examples::Builder& b){
    b.chain(325,"FeatureAggregate",324,examples::transform({{"method",integer(4)}}));
    normalize(b,326,325,dsp_kernels::VectorNormalization::Maximum,0);
    b.chain(327,"QuantileMask",326,examples::transform({{"fraction",NumericLiteral{.6f}}}));
    binary(b,328,326,327,ArrayMathOperation::Multiply,true);binary(b,329,305,328,ArrayMathOperation::Multiply,true);
    b.chain(45,"FeatureAggregate",329,examples::transform({{"method",integer(3)},{"compensated",false}}));
}
void mel_projection(examples::Builder& b,NodeId id,NodeId spectrum,std::span<const float> weights){
    b.chain(id,"FilterMatrixProjection",spectrum,examples::transform({{"weights",NumericArray{std::vector<float>(weights.begin(),weights.end())}},
        {"square_input",true},{"compensated",false}}));
}
NodeId mel_bands(examples::Builder& b,NodeId spectrum,std::span<const float> weights){
    // Each independent projection stays within the public parameter-array limit.
    const auto half=weights.size()/2;mel_projection(b,280,spectrum,weights.first(half));
    mel_projection(b,285,spectrum,weights.subspan(half));
    b.add(286,"ArrayConcat",examples::transform({{"axis",integer(1)}}));
    connect(b,280,286,port(ArrayOutput::Values),port(BinaryInput::Left));
    connect(b,285,286,port(ArrayOutput::Values),port(BinaryInput::Right));b.derive(286);return 286;
}
}
NodeId mel_input(examples::Builder& b,NodeId spectrum){
    std::array<float,42> edges{};rhythm_reference_ports::TriangularSpectralBandConfig config;
    config.weighting=rhythm_reference_ports::TriangleWarp::Log10Mel;
    rhythm_reference_ports::PreparedTriangularSpectralBands projection;
    if(!rhythm_reference_ports::warped_band_edges(edges,0,4000,config.weighting) || !projection.prepare(1025,edges,config))
        throw std::runtime_error("Essentia explicit Mel coefficients");
    const auto bands=mel_bands(b,spectrum,projection.weights());
    b.chain(281,"Clamp",bands,examples::transform({{"minimum",NumericLiteral{1e-10f}}}));
    b.chain(282,"LogCompression",281,examples::transform({{"multiply",NumericLiteral{1.f}},{"add",NumericLiteral{0.f}},{"base",integer(0)}}));
    fill(b,283,282,287,1,40,20);binary(b,284,282,283);return 284;
}
void band_emphasis(examples::Builder& b,NodeId bands,std::size_t frames){band_threshold(b,bands);band_correlation(b,frames);band_selection(b);}
namespace {
struct Link {NodeId node;PortId output,input=0;};
void tracking(examples::Builder& b,NodeId id,std::string_view kind,TransformProcessingParameters parameters,std::initializer_list<Link> links){
    std::vector<OwnedProcessingContract> inputs;for(const auto& link:links)inputs.push_back(b.output(link.node,link.output));
    ConfiguredProcessingNode node;
    const auto status=make_tracking_operation_node(id,kind,inputs,std::move(parameters),b.limits.frame,b.limits.max_owned_metadata_bytes,node);
    if(status!=Status::ok)throw std::runtime_error("Essentia "+std::string(kind)+": id="+std::to_string(id)+" status="+std::to_string(unsigned(status)));
    b.graph.nodes.push_back(std::move(node));
    for(const auto& link:links)connect(b,link.node,id,link.output,link.input);
}
void candidate_curve(examples::Builder& b,NodeId first,NodeId source,PortId curve,float rate,std::size_t up){
    b.chain(first,"VectorNormalize",source,examples::transform({{"method",integer(std::size_t(dsp_kernels::VectorNormalization::Maximum))},{"minimum_scale",NumericLiteral{0.f}}}),curve);
    b.chain(first+1,"LinearExpansion",first,examples::transform({{"factor",integer(up)}}));
    const auto half=std::size_t(std::floor(.1f*rate));
    reindex(b,first+2,first+1,0,0,0,half,half,ArrayPadding::Edge);
    filter(b,first+3,first+2,2*half+1,0,FilterStatePolicy::ResetBatch);
    reindex(b,first+4,first+3,0,2*half);
    binary(b,first+5,first+1,first+4,ArrayMathOperation::Subtract);
    b.chain(first+6,"Clamp",first+5);
    const auto length=std::size_t(std::round(5.944308390022676f*rate));
    b.chain(first+7,"TemporalFrameSequence",first+6,examples::transform({{"window_length",integer(length)},
        {"hop",integer(length/4)},{"center",false},{"tail",integer(1)}}));
}
void candidate_comb(examples::Builder& b,NodeId first,float rate){
    const auto length=std::size_t(std::round(5.944308390022676f*rate)),hop=length/4;
    b.add(first+8,"DirectCorrelation",examples::transform({{"lags",integer(length)},{"normalization",integer(std::size_t(dsp_kernels::CorrelationNormalization::Unbiased))}}));
    connect(b,first+7,first+8,port(ArrayOutput::Values),port(BinaryInput::Left));
    connect(b,first+7,first+8,port(ArrayOutput::Values),port(BinaryInput::Right));b.derive(first+8);
    reindex(b,first+20,first+8,1,0,hop);
    values(b,first+9,rayleigh(hop,std::round(60.f*rate/120.f),true),first+20,first+21,1);
    comb(b,first+10,first+8,first+9,3,(length-3)/4);
    reindex(b,first+11,first+10,1,0,0,8,8,ArrayPadding::Edge);
    filter(b,first+12,first+11,2*std::size_t(std::floor(.1f*rate))+1,1,FilterStatePolicy::ResetBatch,first+3);
    reindex(b,first+13,first+12,1,16);
}
void candidate_observations(examples::Builder& b,NodeId first,float rate){
    binary(b,first+14,first+10,first+13,ArrayMathOperation::Subtract);b.chain(first+15,"Clamp",first+14);
    const auto length=std::size_t(std::round(5.944308390022676f*rate)),hop=length/4;
    const auto low=std::size_t(std::max(0.f,std::floor(60.f*rate/208.f)-1.f));
    const auto high=std::size_t(std::max(0.f,std::ceil(60.f*rate/40.f)-1.f));
    std::vector<float> mask(hop);for(std::size_t i=0;i<hop;++i)mask[i]=float(i>low && i<=high);
    values(b,first+16,mask,first+15,first+22,1);binary(b,first+17,first+15,first+16);
    normalize(b,first+18,first+17,dsp_kernels::VectorNormalization::Sum,1);
}
void candidate_decoders(examples::Builder& b,NodeId first,NodeId output,float frame_rate,std::size_t up){
    const float rate=frame_rate*float(up);const auto length=std::size_t(std::round(5.944308390022676f*rate));
    tracking(b,first+19,"PeriodPath",examples::transform({{"frame_rate",NumericLiteral{rate}},
        {"frame_length",integer(length)},{"period_hop",integer(length/4)},
        {"minimum_bpm",NumericLiteral{40.f}},{"maximum_bpm",NumericLiteral{208.f}}}),{{first+18,port(ArrayOutput::Values),port(PeriodPathInput::Observations)}});
    tracking(b,output,"RenewalBeatDecoder",examples::transform({{"frame_rate",NumericLiteral{frame_rate}},
        {"upsampling",integer(up)},{"maximum_period_s",NumericLiteral{float(length/4)/rate}}}),
        {{first+1,port(ArrayOutput::Values),port(RenewalInput::OnsetCurve)},
         {first+19,port(PeriodPathOutput::Periods),port(RenewalInput::Periods)},{first+19,port(PeriodPathOutput::Ends),port(RenewalInput::Ends)}});
}
}
void candidate(examples::Builder& b,NodeId source,PortId curve,NodeId output,float frame_rate,std::size_t upsampling){
    const NodeId first=400+100*(output-50);const float rate=frame_rate*float(upsampling);
    candidate_curve(b,first,source,curve,rate,upsampling);candidate_comb(b,first,rate);
    candidate_observations(b,first,rate);candidate_decoders(b,first,output,frame_rate,upsampling);
}

}
