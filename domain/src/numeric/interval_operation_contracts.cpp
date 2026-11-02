#include <domain/numeric/interval_operations.h>
#include <domain/graph/processing_model.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace domain {
using namespace signal_contracts;
namespace {
using C=ParameterClassification;using V=ParameterValueKind;
constexpr float default_scales[]{4,8,16,32,64};
constexpr ParameterDescriptor harmonic[]{
    {"reference_bpm",C::Processing,V::Number,1,3.4e38,60,{}},
    {"minimum_common_bpm",C::Processing,V::Number,1,3.4e38,20,{}},
    {"tolerance_percent",C::Processing,V::Number,0,3.4e38,5,{}}};
constexpr ParameterDescriptor sections[]{
    {"tolerance",C::Processing,V::Number,0,1,.08,{}},
    {"long_region_seconds",C::Processing,V::Number,0,3.4e38,20,{}},
    {"short_region_seconds",C::Processing,V::Number,0,3.4e38,4,{}}};
constexpr ParameterDescriptor fluctuation[]{{"scales",C::Processing,V::NumericArray,0,32768,0,{},default_scales}};
struct Geometry {Rank rank;std::size_t rows=1,columns=1;bool variable=false;NumericUnit unit=NumericUnit::Values;};
bool valid_input(const NumericContract& input) noexcept {
    if(input.dtype!=DType::Float32 || input.shape.rank!=Rank::Vector || input.channel_order.size()!=1 ||
       !input.shape.extents[0].maximum || input.representation.encoding!=StorageEncoding::Native || input.representation.valid_bits || input.representation.layout!=ChannelLayout::Planar)return false;
    // Input axes remain validated metadata; these operations do not interpret
    // them as seconds/BPM or forward them to their index/summary outputs.
    return true;
}
std::span<const float> scales(const ConfiguredProcessingNode& node) noexcept {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::NumericExtension,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView value;if(resolve_processing_parameter(view,"scales",value)!=Status::ok)return {};
    const auto* array=std::get_if<std::span<const float>>(&value);return array?*array:std::span<const float>{};
}
bool fluctuation_geometry(const ConfiguredProcessingNode& node,std::array<Geometry,4>& output) noexcept {
    const auto values=scales(node);
    if(values.empty() || !std::is_sorted(values.begin(),values.end()))return false;
    for(float value:values)if(!std::isfinite(value) || value<2 || value>=float(INT32_MAX) || value!=std::floor(value))return false;
    output={Geometry{Rank::Scalar},Geometry{Rank::Vector,values.size()},Geometry{Rank::Vector,values.size()-1},Geometry{Rank::Scalar}};
    return true;
}

bool geometry(const ConfiguredProcessingNode& node,std::size_t maximum,std::array<Geometry,4>& output) noexcept {
    if(node.kind=="BeatIntervalHistogram"){output[0]={Rank::Vector,250};output[1]={Rank::Matrix,2,3};return true;}
    if(node.kind=="HarmonicTempoCandidates"){output[0]={Rank::Vector,maximum,1,true};return true;}
    if(node.kind=="TempoChangeSections"){output[0]=output[1]={Rank::Vector,maximum,1,true,NumericUnit::Seconds};return true;}
    return fluctuation_geometry(node,output);
}
Status own_output(Geometry geometry,OwnedProcessingContract& output) noexcept {
    static constexpr std::uint32_t channel=0;NumericContract value;
    value.channel_order={&channel,1};value.dtype=DType::Float32;value.unit=geometry.unit;value.semantic=Semantic::Numeric;
    value.shape.rank=geometry.rank;value.shape.extents={ExtentBounds{geometry.variable?0:geometry.rows,std::max(std::size_t{1},geometry.rows)},ExtentBounds{geometry.columns,geometry.columns}};
    if(geometry.rank!=Rank::Scalar)value.shape.axes={Axis::Declared,geometry.rank==Rank::Matrix?Axis::Component:Axis::None};
    return own_processing_contract(value,{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,output);
}
}
bool interval_operation_kind(std::string_view kind) noexcept {
    return kind=="BeatIntervalHistogram" || kind=="HarmonicTempoCandidates" || kind=="TempoChangeSections" || kind=="DetrendedFluctuation";
}
std::span<const ParameterDescriptor> interval_operation_parameters(std::string_view kind) noexcept {
    if(kind=="HarmonicTempoCandidates")return harmonic;
    if(kind=="TempoChangeSections")return sections;
    if(kind=="DetrendedFluctuation")return fluctuation;
    return {};
}
std::size_t interval_operation_output_count(std::string_view kind) noexcept {
    if(kind=="HarmonicTempoCandidates")return 1;
    if(kind=="DetrendedFluctuation")return 4;
    return interval_operation_kind(kind)?2:0;
}
Status interval_operation_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,std::vector<OwnedProcessingContract>& output) noexcept {
    if(!interval_operation_kind(node.kind) || !valid_input(input))return Status::incompatible_contract;
    std::array<Geometry,4> shapes{};if(!geometry(node,std::size_t(input.shape.extents[0].maximum),shapes))return Status::invalid_argument;
    std::vector<OwnedProcessingContract> next(interval_operation_output_count(node.kind));
    for(std::size_t i=0;i<next.size();++i){const auto status=own_output(shapes[i],next[i]);if(status!=Status::ok)return status;}
    output=std::move(next);return Status::ok;
}
} // namespace domain
