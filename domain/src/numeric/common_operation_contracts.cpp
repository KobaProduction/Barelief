#include <domain/numeric/common_operations.h>
#include <domain/graph/processing_model.h>
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace domain {
using namespace signal_contracts;
namespace {
constexpr CommonOperationSpec catalogue[]{
    {"TemporalFrameSequence",1,1,"complete input -> unwindowed bounded matrix of overlapping rows"},
    {"ArrayReindex",1,1,"explicit zero-based indices -> selected/permuted vector"},
    {"FeatureAggregate",1,1,"explicit feature vector or matrix rows -> mean/median/sum"},
    {"FilterMatrixProjection",1,3,"explicit band-major weights -> linear projection; weights/centers outputs"},
    {"LogCompression",1,1,"explicit log base/add/multiply on linear values"},
    {"VectorNormalize",1,1,"explicit maximum/minimum/L0/Lp normalization"},
    {"RecursiveFilter",1,2,"explicit normalized difference equation; delay-state output"},
    {"DirectCorrelation",2,1,"explicit pair -> signed lag sums -> selected normalization"},
    {"CurveInterpolation",2,1,"explicit coordinate settings + values + queries -> shared interpolation"},
    {"CombFilterBank",1,2,"explicit delays/alpha/direction -> mono time-by-filter output; delay settings"},
    {"ValueHistogram",1,2,"uniform-range or rounded-bin exact counts; explicit centers"},
    {"InterpolatedPeaks",1,3,"local maxima -> quadratic offsets -> explicit coordinate scale; heights/offsets"},
    {"SameConvolution",1,1,"explicit kernel -> zero-padded same convolution -> explicit crop"},
    {"InverseFourier",2,2,"full complex real/imaginary -> normalized IDFT -> real/imaginary outputs"}};
using C=ParameterClassification;using V=ParameterValueKind;
constexpr ParameterDescriptor framing[]{
    {"window_length",C::Processing,V::Integer,2,65536,2048,{}},{"hop",C::Processing,V::Integer,1,65536,512,{}},
    {"center",C::Processing,V::Boolean,0,1,0,{}},{"tail",C::Processing,V::Integer,0,1,1,{}}};
constexpr ParameterDescriptor reindex[]{{"indices",C::Processing,V::NumericArray,0,0,0,{}}};
constexpr ParameterDescriptor aggregate[]{{"method",C::Processing,V::Integer,1,3,2,{}}};
constexpr ParameterDescriptor projection[]{
    {"weights",C::Processing,V::NumericArray,0,0,0,{}},{"centers_hz",C::Processing,V::NumericArray,0,0,0,{}},
    {"compensated",C::Processing,V::Boolean,0,1,1,{}},{"square_input",C::Processing,V::Boolean,0,1,0,{}},
    {"accumulation",C::Processing,V::String,0,0,0,"separate"}};
constexpr ParameterDescriptor logarithm[]{
    {"multiply",C::Processing,V::Number,0,3.4e38,1,{}},{"add",C::Processing,V::Number,0,3.4e38,1,{}},
    {"base",C::Processing,V::Integer,0,2,0,{}},{"signed_input",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor normalization[]{{"method",C::Processing,V::Integer,0,4,1,{}},{"exponent",C::Processing,V::Number,.000001,1e6,2,{}}};
constexpr ParameterDescriptor recursive[]{
    {"numerator",C::Processing,V::NumericArray,0,0,0,{}},{"denominator",C::Processing,V::NumericArray,0,0,0,{}},
    {"form",C::Processing,V::Integer,0,1,0,{}},{"subnormal",C::Processing,V::Integer,0,2,1,{}},
    {"input_zero_threshold",C::Processing,V::Number,0,3.4e38,0,{}}};
constexpr ParameterDescriptor correlation[]{
    {"first_lag",C::Processing,V::Integer,-65536,65536,0,{}},{"lags",C::Processing,V::Integer,1,65536,512,{}},
    {"normalization",C::Processing,V::Integer,0,3,0,{}}};
constexpr ParameterDescriptor interpolation[]{
    {"coordinates",C::Processing,V::NumericArray,0,0,0,{}},{"method",C::Processing,V::Integer,0,8,0,{}},
    {"spline_order",C::Processing,V::Integer,0,16,1,{}},{"fill_below",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},
    {"fill_above",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},{"extrapolate",C::Processing,V::Boolean,0,1,0,{}},
    {"bounds_error",C::Processing,V::Boolean,0,1,0,{}},{"keep_infinite_coordinates",C::Processing,V::Boolean,0,1,0,{}},
    {"source_linear",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor comb[]{
    {"delays",C::Processing,V::NumericArray,0,0,0,{}},{"alpha",C::Processing,V::NumericArray,0,0,0,{}},
    {"direction",C::Processing,V::Integer,0,1,1,{}}};
constexpr ParameterDescriptor histogram[]{
    {"bins",C::Processing,V::Integer,2,65536,16,{}},{"method",C::Processing,V::Integer,0,1,0,{}},
    {"step",C::Processing,V::Number,.000001,3.4e38,1,{}}};
constexpr ParameterDescriptor peaks[]{
    {"threshold",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},{"endpoints",C::Processing,V::Boolean,0,1,0,{}},
    {"origin",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},{"step",C::Processing,V::Number,.000001,3.4e38,1,{}}};
constexpr ParameterDescriptor convolution[]{
    {"kernel",C::Processing,V::NumericArray,0,0,0,{}},{"crop",C::Processing,V::Integer,0,1,0,{}}};
constexpr ParameterDescriptor inverse[]{{"fft_size",C::Processing,V::Integer,1,65536,512,{}}};
const ProcessingParameterValue* parameter(const ConfiguredProcessingNode& n,std::string_view key) noexcept {
    const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
    if(p)for(const auto& f:p->values)if(f.key==key)return &f.value;
    return nullptr;
}
std::span<const float> array(const ConfiguredProcessingNode& n,std::string_view key) noexcept {
    const auto* p=parameter(n,key);const auto* a=p?std::get_if<NumericArray>(p):nullptr;
    return a?std::span<const float>(a->values):std::span<const float>{};
}
bool size(const ConfiguredProcessingNode& n,std::string_view key,std::size_t fallback,std::size_t& output) noexcept {
    const auto* p=parameter(n,key);if(!p){output=fallback;return true;}
    const auto* l=std::get_if<NumericLiteral>(p);if(!l)return false;
    return std::visit([&](auto v){if constexpr(std::is_integral_v<decltype(v)>){if(!std::in_range<std::size_t>(v))return false;output=std::size_t(v);return true;}else return false;},*l);
}
bool projection_policy(const ConfiguredProcessingNode& node) noexcept {
    const auto* field=parameter(node,"accumulation");
    const auto* mode=field?std::get_if<std::string>(field):nullptr;
    if(field && !mode)return false;
    if(!mode || *mode=="separate")return true;
    if(*mode!="sequential_fma")return false;
    const auto* setting=parameter(node,"compensated");const auto* compensated=setting?std::get_if<bool>(setting):nullptr;
    return compensated && !*compensated;
}
struct Geometry {Rank rank=Rank::Vector;std::size_t rows=1,columns=1;bool variable=false;DType type=DType::Float32;};
bool filter_geometry(const ConfiguredProcessingNode& n,const NumericContract& in,std::array<Geometry,3>& shapes) noexcept {
    if(!projection_policy(n))return false;
    const auto bins=std::size_t(in.shape.extents[in.shape.rank==Rank::Matrix?1:0].maximum);
    const auto weights=array(n,"weights");if(!bins || weights.empty() || weights.size()%bins)return false;
    const auto bands=weights.size()/bins;
    shapes[0]={in.shape.rank,in.shape.rank==Rank::Matrix?std::size_t(in.shape.extents[0].maximum):bands,in.shape.rank==Rank::Matrix?bands:1,in.shape.rank==Rank::Matrix};
    shapes[1]={Rank::Matrix,bands,bins};shapes[2]={Rank::Vector,bands,1,true};return true;
}
bool recursive_shape(const ConfiguredProcessingNode& n,std::array<Geometry,3>& out) noexcept {
    std::size_t form=0;if(!size(n,"form",0,form) || form>1)return false;
    const auto count=std::max({std::size_t{1},array(n,"numerator").size(),array(n,"denominator").size()});
    if(count>SIZE_MAX/(form?2u:1u))return false;
    out[1]={Rank::Vector,count*(form?2u:1u)};return true;
}
bool interpolation_shape(const ConfiguredProcessingNode& n,std::array<Geometry,3>& out) noexcept {
    for(const auto& p:n.inputs)if(p.id==1){
        const auto c=p.contract.view();const auto* value=std::get_if<NumericContract>(&c);
        if(!value || value->shape.rank!=Rank::Vector || value->shape.extents[0].maximum>SIZE_MAX)return false;
        out[0].rows=std::size_t(value->shape.extents[0].maximum);return true;
    }
    return false;
}
bool value_histogram_shape(const ConfiguredProcessingNode& n,std::array<Geometry,3>& out) noexcept {
    std::size_t bins=0;
    if(!size(n,"bins",16,bins) || bins<2)return false;
    out[0]={Rank::Vector,bins,1,false,DType::UInt64};
    out[1]={Rank::Vector,bins};
    return true;
}

bool statistical_shape(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    if(n.kind=="ValueHistogram")return value_histogram_shape(n,out);
    if(n.kind=="CombFilterBank"){
        const auto filters=array(n,"delays").size();if(!filters)return false;
        out[0]={Rank::Matrix,count,filters,true};out[1]={Rank::Vector,filters};return true;
    }
    if(n.kind=="InterpolatedPeaks"){out[1]=out[2]=out[0];return true;}
    return n.kind=="LogCompression" || n.kind=="VectorNormalize";
}
bool convolution_shape(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    std::size_t crop=0;if(!size(n,"crop",0,crop) || crop>1)return false;
    out[0].rows=crop?count:std::max(count,array(n,"kernel").size());return true;
}
bool inverse_fourier_shape(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    std::size_t fft=0;
    if(!size(n,"fft_size",512,fft) || fft!=count)return false;
    out[1]=out[0];
    return true;
}

std::size_t initialize_vector_geometry(const NumericContract& input,Geometry& geometry) noexcept {
    const auto count=std::size_t(input.shape.extents[0].maximum);
    geometry={Rank::Vector,count,1,true};
    return count;
}
bool value_shapes(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    if(n.kind=="ArrayReindex"){out[0]={Rank::Vector,array(n,"indices").size(),1,false};return out[0].rows!=0;}
    if(n.kind=="RecursiveFilter")return recursive_shape(n,out);
    if(n.kind=="DirectCorrelation")return size(n,"lags",count,out[0].rows) && out[0].rows;
    if(n.kind=="CurveInterpolation")return interpolation_shape(n,out);
    if(n.kind=="SameConvolution")return convolution_shape(n,count,out);
    return statistical_shape(n,count,out);
}
bool frame_sequence_shape(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    std::size_t length=0,hop=0,tail=0;
    if(!size(n,"window_length",2048,length) || length<2 || !size(n,"hop",512,hop) || !hop || !size(n,"tail",1,tail) || tail>1)return false;
    const auto* centered=parameter(n,"center");
    if(centered && (!std::holds_alternative<bool>(*centered) || std::get<bool>(*centered)))return false;
    const auto rows=tail?(count<=length?std::size_t(count!=0):1+(count-length)/hop+std::size_t((count-length)%hop!=0)):(count<length?0:1+(count-length)/hop);
    const auto bounded=tail && count?std::min(rows,1+(count-1)/hop):rows;
    out[0]={Rank::Matrix,bounded,length,true};return bounded!=0;
}
bool shape(const ConfiguredProcessingNode& n,const NumericContract& in,std::array<Geometry,3>& out) noexcept {
    const auto count=initialize_vector_geometry(in,out[0]);
    if(n.kind=="FeatureAggregate"){
        out[0]=in.shape.rank==Rank::Matrix?Geometry{Rank::Vector,count,1,true}:Geometry{Rank::Scalar,1,1,false};return true;
    }
    if(n.kind=="ArrayReindex" && in.shape.rank==Rank::Matrix){out[0]={Rank::Matrix,count,array(n,"indices").size(),true};return out[0].columns!=0;}
    if(n.kind=="TemporalFrameSequence")return frame_sequence_shape(n,count,out);
    if(n.kind=="FilterMatrixProjection")return filter_geometry(n,in,out);
    if(n.kind=="InverseFourier")return inverse_fourier_shape(n,count,out);
    return value_shapes(n,count,out);
}
bool signed_integer(const ConfiguredProcessingNode& n,std::string_view key,std::int64_t& output) noexcept {
    const auto* p=parameter(n,key);if(!p){output=0;return true;}
    const auto* value=std::get_if<NumericLiteral>(p);if(!value)return false;
    return std::visit([&](auto x){if constexpr(std::is_integral_v<decltype(x)>){if(!std::in_range<std::int64_t>(x))return false;output=std::int64_t(x);return true;}else return false;},*value);
}
void retain_projection_axes(const ConfiguredProcessingNode& node,const NumericContract& input,NumericContract& value) noexcept {
    value.clock=input.clock;value.channel_order=input.channel_order;
    if(input.shape.rank==Rank::Matrix){
        value.shape.axes[0]=input.shape.axes[0];
        value.coordinates[0]=input.coordinates[0];
        value.shape.extents[0]=input.shape.extents[0];
    }
    if(!array(node,"centers_hz").empty())value.coordinates[input.shape.rank==Rank::Matrix?1:0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
    value.unit=input.unit;
}

void retain_reindex_axes(const NumericContract& input,NumericContract& value) noexcept {
    value.unit=input.unit;value.quantity=input.quantity;
    if(input.shape.rank!=Rank::Matrix)return;
    value.shape.extents[0]=input.shape.extents[0];value.shape.axes[0]=input.shape.axes[0];value.coordinates[0]=input.coordinates[0];
}
void retain_axes(const ConfiguredProcessingNode& node,std::size_t port,const NumericContract& input,NumericContract& value) noexcept {
    if(port)return;
    if(node.kind=="LogCompression" || node.kind=="VectorNormalize" || node.kind=="RecursiveFilter"){
        value.shape=input.shape;value.coordinates=input.coordinates;value.clock=input.clock;value.channel_order=input.channel_order;
        if(node.kind=="RecursiveFilter")value.unit=input.unit;
    }
    if(node.kind=="ArrayReindex")retain_reindex_axes(input,value);
    if(node.kind=="FilterMatrixProjection")retain_projection_axes(node,input,value);
}
NumericContract output_base(Geometry geometry) {
    static constexpr std::uint32_t mono=0;NumericContract value;value.dtype=geometry.type;value.channel_order={&mono,1};value.semantic=Semantic::Numeric;value.unit=NumericUnit::Values;
    value.shape={geometry.rank,{Axis::Declared,geometry.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{geometry.variable?0:geometry.rows,std::max(std::size_t{1},geometry.rows)},ExtentBounds{geometry.columns,geometry.columns}}};
    if(geometry.rank==Rank::Scalar)value.shape={};
    return value;
}
Status own(const ConfiguredProcessingNode& node,std::size_t port,const NumericContract& input,Geometry geometry,OwnedProcessingContract& output) noexcept {
    auto value=output_base(geometry);
    retain_axes(node,port,input,value);
    if(node.kind=="FeatureAggregate" || node.kind=="ArrayReindex" || node.kind=="TemporalFrameSequence")value.clock=input.clock;
    if(node.kind=="FilterMatrixProjection" && port==2)value.unit=NumericUnit::Hertz;
    if(node.kind=="DirectCorrelation"){
        std::int64_t first=0;if(!signed_integer(node,"first_lag",first))return Status::invalid_argument;
        value.coordinates[0]={CoordinateMode::Regular,NumericUnit::Frames,{first,1},{1,1}};
    }
    return own_processing_contract(value,{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,output);
}
}
std::span<const CommonOperationSpec> common_operation_catalogue() noexcept {return catalogue;}
const CommonOperationSpec* common_operation_spec(std::string_view kind) noexcept {for(const auto& s:catalogue)if(s.kind==kind)return &s;return nullptr;}
bool common_operation_kind(std::string_view kind) noexcept {return common_operation_spec(kind)!=nullptr;}
std::span<const ParameterDescriptor> common_operation_parameters(std::string_view kind) noexcept {
    struct Parameters {std::string_view kind;std::span<const ParameterDescriptor> values;};
    const Parameters sets[]{{"TemporalFrameSequence",framing},{"ArrayReindex",reindex},{"FeatureAggregate",aggregate},{"FilterMatrixProjection",projection},{"LogCompression",logarithm},{"VectorNormalize",normalization},
        {"RecursiveFilter",recursive},{"DirectCorrelation",correlation},{"CurveInterpolation",interpolation},
        {"CombFilterBank",comb},{"ValueHistogram",histogram},{"InterpolatedPeaks",peaks},{"SameConvolution",convolution},{"InverseFourier",inverse}};
    for(const auto& set:sets)if(set.kind==kind)return set.values;
    return {};
}
Status common_operation_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,std::vector<OwnedProcessingContract>& output) noexcept {
    const auto* spec=common_operation_spec(node.kind);
    if(!spec || input.dtype!=DType::Float32 || input.channel_order.size()!=1 || (input.shape.rank!=Rank::Vector && input.shape.rank!=Rank::Matrix) || input.shape.extents[0].maximum>SIZE_MAX || input.shape.extents[1].maximum>SIZE_MAX)return Status::incompatible_contract;
    std::array<Geometry,3> shapes;if(!shape(node,input,shapes))return Status::invalid_argument;
    std::vector<OwnedProcessingContract> next(spec->outputs);
    for(std::size_t k=0;k<next.size();++k){const auto status=own(node,k,input,shapes[k],next[k]);if(status!=Status::ok)return status;}
    output=std::move(next);return Status::ok;
}
}
