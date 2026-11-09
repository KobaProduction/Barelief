#include <domain/numeric/common_operations.h>
#include <domain/numeric/array_processing_ports.h>
#include <domain/graph/processing_model.h>
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace domain {
using namespace signal_contracts;
namespace {
constexpr CommonOperationSpec catalogue[]{
    {"AxisMaximum",1,3,"first maximum index, maximum value and all tied maximum values along an explicit axis"},
    {"AxisSum",1,1,"explicit sequential or pairwise sum along an axis; cumulative sums are sequential"},
    {"ArrayGather",2,1,"explicit table and UInt64 indices -> gathered values"},
    {"NumericMap",1,1,"explicit scalar scale, affine offset, identity/reciprocal and output dtype"},
    {"OrderedPeaks",1,2,"strict wrapped local maxima -> descending height, descending index tie order"},
    {"AbsoluteValue",1,1,"numeric scalar/vector/matrix -> absolute values; same dtype and geometry"},
    {"TemporalFrameSequence",1,1,"complete input -> unwindowed bounded matrix of overlapping rows"},
    {"FeatureAggregate",1,1,"explicit feature vector or matrix rows -> mean/median/sum"},
    {"FilterMatrixProjection",1,3,"explicit band-major weights -> linear projection; weights/centers outputs"},
    {"LinearExpansion",1,1,"explicit integer factor -> endpoint-preserving linear interpolation"},
    {"QuantileMask",1,1,"explicit order-statistic quantile -> inclusive upper-tail numeric mask"},
    {"Clamp",1,1,"explicit inclusive minimum/maximum on numeric arrays"},
    {"LogCompression",1,1,"explicit log base/add/multiply on linear values"},
    {"VectorNormalize",1,1,"explicit maximum/minimum/L0/Lp/sum/deviation normalization along selected axis"},
    {"RecursiveFilter",1,2,"explicit normalized difference equation; initial/final delay state; batch/row reset",1},
    {"DirectCorrelation",2,1,"explicit pair -> signed lag sums -> selected normalization"},
    {"CurveInterpolation",2,1,"explicit coordinate settings + values + queries -> shared interpolation"},
    {"CombFilterBank",1,2,"explicit delays/alpha/direction -> mono time-by-filter output; delay settings"},
    {"ValueHistogram",1,2,"uniform-range or rounded-bin exact counts; explicit centers"},
    {"InterpolatedPeaks",1,3,"local maxima -> quadratic offsets -> explicit coordinate scale; heights/offsets"},
    {"SameConvolution",1,1,"explicit kernel -> zero-padded same convolution along an explicit axis",1},
    {"InverseFourier",2,2,"full complex real/imaginary -> normalized IDFT -> real/imaginary outputs"}};
using C=ParameterClassification;using V=ParameterValueKind;
constexpr ParameterDescriptor framing[]{
    {"window_length",C::Processing,V::Integer,2,65536,2048,{}},{"hop",C::Processing,V::Integer,1,65536,512,{}},
    {"center",C::Processing,V::Boolean,0,1,0,{}},{"tail",C::Processing,V::Integer,0,1,1,{}}};
constexpr ParameterDescriptor aggregate[]{{"method",C::Processing,V::Integer,1,4,2,{}},{"compensated",C::Processing,V::Boolean,0,1,1,{}}};
constexpr ParameterDescriptor projection[]{
    {"weights",C::Processing,V::NumericArray,0,0,0,{}},{"centers_hz",C::Processing,V::NumericArray,0,0,0,{}},
    {"compensated",C::Processing,V::Boolean,0,1,1,{}},{"square_input",C::Processing,V::Boolean,0,1,0,{}},
    {"accumulation",C::Processing,V::String,0,0,0,"separate"}};
constexpr ParameterDescriptor logarithm[]{
    {"multiply",C::Processing,V::Number,0,3.4e38,1,{}},{"add",C::Processing,V::Number,0,3.4e38,1,{}},
    {"base",C::Processing,V::Integer,0,2,0,{}},{"signed_input",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor expansion[]{{"factor",C::Processing,V::Integer,1,4,2,{}}};
constexpr ParameterDescriptor quantile[]{{"fraction",C::Processing,V::Number,0,.999999,0.6,{}}};
constexpr ParameterDescriptor clamp[]{
    {"minimum",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},{"maximum",C::Processing,V::Number,-3.4e38,3.4028234663852886e38,3.4028234663852886e38,{}}};
constexpr ParameterDescriptor normalization[]{
    {"method",C::Processing,V::Integer,0,6,1,{}},{"exponent",C::Processing,V::Number,.000001,1e6,2,{}},
    {"axis",C::Processing,V::Integer,0,2,2,{}},{"minimum_scale",C::Processing,V::Number,0,3.4e38,1.1754943508222875e-38,{}}};
constexpr ParameterDescriptor recursive[]{
    {"numerator",C::Processing,V::NumericArray,0,0,0,{}},{"denominator",C::Processing,V::NumericArray,0,0,0,{}},
    {"form",C::Processing,V::Integer,0,1,0,{}},{"subnormal",C::Processing,V::Integer,0,2,1,{}},
    {"input_zero_threshold",C::Processing,V::Number,0,3.4e38,0,{}},
    {"axis",C::Processing,V::Integer,0,2,2,{}},{"state_policy",C::Processing,V::Integer,0,2,0,{}}};
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
    {"direction",C::Processing,V::Integer,0,1,1,{}},{"reset_per_call",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor histogram[]{
    {"bins",C::Processing,V::Integer,2,65536,16,{}},{"method",C::Processing,V::Integer,0,1,0,{}},
    {"step",C::Processing,V::Number,.000001,3.4e38,1,{}}};
constexpr ParameterDescriptor peaks[]{
    {"threshold",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},{"endpoints",C::Processing,V::Boolean,0,1,0,{}},
    {"origin",C::Processing,V::Number,-3.4e38,3.4e38,0,{}},{"step",C::Processing,V::Number,.000001,3.4e38,1,{}}};
constexpr ParameterDescriptor axis_parameters[]{{"axis",C::Processing,V::Integer,0,2,2,{}}};
constexpr ParameterDescriptor sum_parameters[]{
    {"axis",C::Processing,V::Integer,0,2,2,{}},{"cumulative",C::Processing,V::Boolean,0,1,0,{}},
    {"accumulation",C::Processing,V::Integer,0,1,0,{}}};
constexpr ParameterDescriptor map_parameters[]{
    {"operation",C::Processing,V::Integer,0,1,0,{}},{"scale",C::Processing,V::Number,-3.4e38,3.4e38,1,{}},
    {"offset",C::Processing,V::Number,-std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),0,{}},
    {"output_dtype",C::Processing,V::Integer,0,2,0,{}}};
constexpr ParameterDescriptor ordered_peak_parameters[]{{"maximum_count",C::Processing,V::Integer,1,65536,5,{}}};
constexpr ParameterDescriptor convolution[]{
    {"kernel",C::Processing,V::NumericArray,0,0,0,{}},{"crop",C::Processing,V::Integer,0,1,0,{}},{"axis",C::Processing,V::Integer,0,2,2,{}}};
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
    return n.kind=="LogCompression" || n.kind=="VectorNormalize" || n.kind=="Clamp" || n.kind=="QuantileMask";
}
bool convolution_shape(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    std::size_t crop=0;if(!size(n,"crop",0,crop) || crop>1)return false;
    out[0].rows=crop?count:std::max(count,array(n,"kernel").size());return true;
}
bool inverse_fourier_shape(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    std::size_t fft=0;
    if(!size(n,"fft_size",512,fft) || fft!=count)return false;
    out[0].variable=false;
    out[1]=out[0];
    return true;
}

std::size_t initialize_vector_geometry(const NumericContract& input,Geometry& geometry) noexcept {
    const auto count=std::size_t(input.shape.extents[0].maximum);
    geometry={Rank::Vector,count,1,true};
    return count;
}
bool matrix_shape(const ConfiguredProcessingNode& node,const NumericContract& input,std::array<Geometry,3>& out) noexcept {
    out[0]={Rank::Matrix,std::size_t(input.shape.extents[0].maximum),std::size_t(input.shape.extents[1].maximum),true};
    if(node.kind=="RecursiveFilter")return recursive_shape(node,out);
    if(node.kind=="DirectCorrelation")return size(node,"lags",out[0].columns,out[0].columns) && out[0].columns;
    return inverse_fourier_shape(node,out[0].columns,out);
}
bool expansion_shape(const ConfiguredProcessingNode& n,std::size_t count,Geometry& out) noexcept {
    std::size_t factor=0;if(!size(n,"factor",2,factor) || factor<1 || factor>4 || !count || count-1>(SIZE_MAX-1)/factor)return false;
    out.rows=(count-1)*factor+1;return true;
}
bool value_shapes(const ConfiguredProcessingNode& n,std::size_t count,std::array<Geometry,3>& out) noexcept {
    if(n.kind=="LinearExpansion")return expansion_shape(n,count,out[0]);
    if(n.kind=="RecursiveFilter")return recursive_shape(n,out);
    if(n.kind=="DirectCorrelation"){
        out[0].variable=false;
        return size(n,"lags",count,out[0].rows) && out[0].rows;
    }
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
    if(in.shape.rank==Rank::Matrix && (n.kind=="RecursiveFilter" ||
       n.kind=="DirectCorrelation" || n.kind=="InverseFourier"))return matrix_shape(n,in,out);
    if(n.kind=="TemporalFrameSequence")return frame_sequence_shape(n,count,out);
    if(n.kind=="FilterMatrixProjection")return filter_geometry(n,in,out);
    if(n.kind=="InverseFourier")return inverse_fourier_shape(n,count,out);
    return value_shapes(n,count,out);
}
bool axis_kind(std::string_view kind) noexcept {
    return kind=="AxisMaximum" || kind=="AxisSum" || kind=="ArrayGather" || kind=="NumericMap" || kind=="OrderedPeaks" || kind=="SameConvolution";
}
bool axis_map_shape(const ConfiguredProcessingNode& n,Geometry& output) noexcept {
    std::size_t type=0;if(!size(n,"output_dtype",0,type) || type>2)return false;
    output.type=type==2?DType::UInt64:type==1?DType::Float64:DType::Float32;return true;
}
bool axis_sum_shape(const ConfiguredProcessingNode& n, std::size_t other, Geometry& output) noexcept {
    const auto* cumulative = parameter(n, "cumulative");
    if (cumulative && !std::holds_alternative<bool>(*cumulative)) {
        return false;
    }
    if (!cumulative || !std::get<bool>(*cumulative)) {
        output = {Rank::Vector, other, 1, true, output.type};
    }
    return true;
}
bool ordered_peak_shapes(const ConfiguredProcessingNode& n,std::array<Geometry,3>& out) noexcept {
    std::size_t count=0;if(out[0].rank!=Rank::Vector || !size(n,"maximum_count",5,count) || !count)return false;
    out[1]={Rank::Vector,std::min(out[0].rows,count),1,true,out[0].type};
    out[0]={Rank::Vector,out[1].rows,1,true,DType::UInt64};return true;
}
bool axis_convolution_shape(const ConfiguredProcessingNode& n,std::size_t axis,Geometry& out) noexcept {
    std::size_t crop=0;if(!size(n,"crop",0,crop) || crop>1)return false;
    auto kernel=array(n,"kernel").size();
    for(const auto& port:n.inputs)if(port.id==1){
        const auto view=port.contract.view();const auto* c=std::get_if<NumericContract>(&view);
        if(!c || c->shape.rank!=Rank::Vector || c->dtype!=out.type)return false;
        kernel=std::size_t(c->shape.extents[0].maximum);
    }
    auto& width=axis?out.columns:out.rows;width=crop?width:std::max(width,kernel);return true;
}
bool maximum_shapes(std::size_t count,std::array<Geometry,3>& out) noexcept {
    out[2]=out[0];out[1]={Rank::Vector,count,1,true,out[0].type};out[0]={Rank::Vector,count,1,true,DType::UInt64};return true;
}
bool selected_array_axis(const ConfiguredProcessingNode& node,Rank rank,std::size_t& axis) noexcept {
    const auto last=std::size_t(ArrayAxis::Last);std::size_t selected=last;
    if(!size(node,"axis",last,selected) || selected>last)return false;
    axis=selected==last?(rank==Rank::Matrix?1u:0u):selected;
    return rank==Rank::Matrix || !axis;
}
bool selected_axis_shapes(const ConfiguredProcessingNode& n,const NumericContract& input,std::size_t axis,std::array<Geometry,3>& out) noexcept {
    const auto other=input.shape.rank==Rank::Matrix?(axis?out[0].rows:out[0].columns):1;
    if(n.kind=="AxisMaximum")return maximum_shapes(other,out);
    if(n.kind=="AxisSum")return axis_sum_shape(n,other,out[0]);
    if(n.kind=="NumericMap")return axis_map_shape(n,out[0]);
    if(n.kind=="OrderedPeaks")return ordered_peak_shapes(n,out);
    if(n.kind=="ArrayGather")return input.shape.rank==Rank::Vector && interpolation_shape(n,out);
    return axis_convolution_shape(n,axis,out[0]);
}
bool axis_shapes(const ConfiguredProcessingNode& n,const NumericContract& input,std::array<Geometry,3>& out) noexcept {
    out[0]={input.shape.rank,std::size_t(input.shape.extents[0].maximum),std::size_t(input.shape.extents[1].maximum),true,input.dtype};
    std::size_t axis=0;
    return selected_array_axis(n,input.shape.rank,axis) && selected_axis_shapes(n,input,axis,out);
}
bool operation_shapes(const ConfiguredProcessingNode& node,const NumericContract& input,std::array<Geometry,3>& out) noexcept {
    if(node.kind!="CombFilterBank")return axis_kind(node.kind)?axis_shapes(node,input,out):shape(node,input,out);
    const auto filters=array(node,"delays").size();if(!filters || input.shape.rank!=Rank::Vector)return false;
    out[0]={Rank::Matrix,std::size_t(input.shape.extents[0].maximum),filters,true,input.dtype};
    out[1]={Rank::Vector,filters};return true;
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

void retain_pointwise_axes(const ConfiguredProcessingNode& node,const NumericContract& input,NumericContract& value) noexcept {
    if(node.kind=="Clamp" || node.kind=="NumericMap" || node.kind=="LogCompression" || node.kind=="VectorNormalize" || node.kind=="RecursiveFilter"){
        value.shape=input.shape;value.coordinates=input.coordinates;value.clock=input.clock;value.channel_order=input.channel_order;
        if(node.kind=="RecursiveFilter")value.unit=input.unit;
    }
}
void retain_axes(const ConfiguredProcessingNode& node,std::size_t port,const NumericContract& input,NumericContract& value) noexcept {
    if(port)return;
    retain_pointwise_axes(node,input,value);
    if(node.kind=="FilterMatrixProjection")retain_projection_axes(node,input,value);
    if(input.shape.rank==Rank::Matrix && (node.kind=="DirectCorrelation" || node.kind=="InverseFourier")){
        value.clock=input.clock;value.shape.extents[0]=input.shape.extents[0];
        value.shape.axes[0]=input.shape.axes[0];value.coordinates[0]=input.coordinates[0];
    }
}
NumericContract output_base(Geometry geometry) {
    static constexpr std::uint32_t mono=0;NumericContract value;value.dtype=geometry.type;value.channel_order={&mono,1};value.semantic=Semantic::Numeric;value.unit=NumericUnit::Values;
    value.shape={geometry.rank,{Axis::Declared,geometry.rank==Rank::Matrix?Axis::Component:Axis::None},
        {ExtentBounds{geometry.variable?0:geometry.rows,std::max(std::size_t{1},geometry.rows)},ExtentBounds{geometry.columns,geometry.columns}}};
    if(geometry.rank==Rank::Scalar)value.shape={};
    return value;
}
void retain_gather_clock(const ConfiguredProcessingNode& node,NumericContract& output) noexcept {
    for(const auto& port:node.inputs)if(port.id==1){
        const auto view=port.contract.view();const auto* indices=std::get_if<NumericContract>(&view);
        if(indices)output.clock=indices->clock;
    }
}
Status own(const ConfiguredProcessingNode& node,std::size_t port,const NumericContract& input,Geometry geometry,OwnedProcessingContract& output) noexcept {
    auto value=output_base(geometry);
    retain_axes(node,port,input,value);
    if(node.kind=="ArrayGather"){value.unit=input.unit;retain_gather_clock(node,value);}
    if(node.kind=="FeatureAggregate" || node.kind=="TemporalFrameSequence" || node.kind=="DirectCorrelation" || node.kind=="InverseFourier")value.clock=input.clock;
    if(node.kind=="FilterMatrixProjection" && port==2)value.unit=NumericUnit::Hertz;
    if(node.kind=="DirectCorrelation"){
        std::int64_t first=0;if(!signed_integer(node,"first_lag",first))return Status::invalid_argument;
        value.coordinates[input.shape.rank==Rank::Matrix?1:0]={CoordinateMode::Regular,NumericUnit::Frames,{first,1},{1,1}};
    }
    return own_processing_contract(value,{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,output);
}
}
std::span<const CommonOperationSpec> common_operation_catalogue() noexcept {return catalogue;}
const CommonOperationSpec* common_operation_spec(std::string_view kind) noexcept {for(const auto& s:catalogue)if(s.kind==kind)return &s;return nullptr;}
bool common_operation_kind(std::string_view kind) noexcept {return common_operation_spec(kind)!=nullptr;}
std::span<const ParameterDescriptor> common_operation_parameters(std::string_view kind) noexcept {
    struct Parameters {std::string_view kind;std::span<const ParameterDescriptor> values;};
    const Parameters sets[]{{"AxisMaximum",axis_parameters},{"AxisSum",sum_parameters},{"NumericMap",map_parameters},{"OrderedPeaks",ordered_peak_parameters},{"TemporalFrameSequence",framing},{"FeatureAggregate",aggregate},{"FilterMatrixProjection",projection},{"LinearExpansion",expansion},{"Clamp",clamp},{"QuantileMask",quantile},{"LogCompression",logarithm},{"VectorNormalize",normalization},
        {"RecursiveFilter",recursive},{"DirectCorrelation",correlation},{"CurveInterpolation",interpolation},
        {"CombFilterBank",comb},{"ValueHistogram",histogram},{"InterpolatedPeaks",peaks},{"SameConvolution",convolution},{"InverseFourier",inverse}};
    for(const auto& set:sets)if(set.kind==kind)return set.values;
    return {};
}
namespace {
Status absolute_output_contract(const NumericContract& input, std::vector<OwnedProcessingContract>& output) {
    if ((input.dtype != DType::Float32 && input.dtype != DType::Float64) || input.channel_order.size() != 1 ||
        input.representation.encoding != StorageEncoding::Native || input.representation.layout != ChannelLayout::Planar) {
        return Status::incompatible_contract;
    }
    auto value = input;
    value.representation.normalization = Normalization::None;
    std::vector<OwnedProcessingContract> next(1);
    const auto status = own_processing_contract(value, {1, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX}, SIZE_MAX, next[0]);
    if (status == Status::ok) {
        output = std::move(next);
    }
    return status;
}
}
namespace {
bool common_input_dtype(std::string_view kind,DType dtype) noexcept {
    return dtype==DType::Float32 || (dtype==DType::Float64 && (axis_kind(kind) || kind=="CombFilterBank"));
}
bool common_input_geometry(std::string_view kind,const NumericContract& input) noexcept {
    const auto rank=input.shape.rank;
    return input.channel_order.size()==1 && (rank==Rank::Vector || rank==Rank::Matrix || (kind=="NumericMap" && rank==Rank::Scalar)) &&
        input.shape.extents[0].maximum<=SIZE_MAX && input.shape.extents[1].maximum<=SIZE_MAX;
}
Status own_common_outputs(const ConfiguredProcessingNode& node,const NumericContract& input,std::span<const Geometry> shapes,std::vector<OwnedProcessingContract>& next) noexcept {
    for(std::size_t k=0;k<next.size();++k){const auto status=own(node,k,input,shapes[k],next[k]);if(status!=Status::ok)return status;}
    return Status::ok;
}
}
Status common_operation_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,std::vector<OwnedProcessingContract>& output) noexcept {
    if(node.kind=="AbsoluteValue")return absolute_output_contract(input,output);
    const auto* spec=common_operation_spec(node.kind);
    if(!spec || !common_input_dtype(node.kind,input.dtype) || !common_input_geometry(node.kind,input))return Status::incompatible_contract;
    std::array<Geometry,3> shapes;if(!operation_shapes(node,input,shapes))return Status::invalid_argument;
    std::vector<OwnedProcessingContract> next(spec->outputs);
    const auto status=own_common_outputs(node,input,shapes,next);if(status!=Status::ok)return status;
    output=std::move(next);return Status::ok;
}
}
