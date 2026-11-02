#include <domain/numeric/temporal_operations.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace domain {
using namespace signal_contracts;
namespace {
using C=ParameterClassification;using V=ParameterValueKind;
constexpr ParameterDescriptor rates[]{
    {"input_rate",C::Processing,V::Number,.000001,192000,120,{}},
    {"sample_rate",C::Processing,V::Number,0,192000,0,{}},
    {"hop_samples",C::Processing,V::Integer,1,65536,512,{}},
    {"input_origin_s",C::Processing,V::Number,-1e9,1e9,0,{}}};
constexpr ParameterDescriptor windows[]{
    {"window_length",C::Processing,V::Integer,2,16384,384,{}},
    {"hop",C::Processing,V::Integer,1,65536,1,{}},
    {"function",C::Processing,V::WindowFunction,0,0,0,"hann"},
    {"coefficients",C::Processing,V::NumericArray,0,0,0,{}},
    {"periodic",C::Processing,V::Boolean,0,1,1,{}},
    {"center",C::Processing,V::Boolean,0,1,1,{}},
    {"tail",C::Processing,V::Integer,0,3,0,{}},
    {"odd_center_ceil",C::Processing,V::Boolean,0,1,0,{}},
    {"empty_centered_frame",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor positions[]{
    {"frame_index",C::Processing,V::Integer,0,65536,0,{}},
    {"use_first_sample",C::Processing,V::Boolean,0,1,0,{}},
    {"first_sample",C::Processing,V::Integer,-1e9,1e9,0,{}}};
constexpr ParameterDescriptor frame_index[]{{"frame_index",C::Processing,V::Integer,0,65536,0,{}}};
constexpr ParameterDescriptor padding[]{{"padding",C::Processing,V::Integer,0,4,0,{}}};
constexpr ParameterDescriptor ramp[]{{"padding",C::Processing,V::Integer,0,4,1,{}}};
constexpr ParameterDescriptor sequence_ramp[]{{"acf_padding",C::Processing,V::Integer,0,4,1,{}}};
constexpr ParameterDescriptor fft[]{
    {"fft_size",C::Processing,V::Integer,0,65536,0,{}},
    {"rotation",C::Processing,V::Integer,-1e9,1e9,0,{}},
    {"zero_phase",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor acf_fft[]{
    {"fft_size",C::Processing,V::Integer,0,65536,0,{}},
    {"fft_length",C::Processing,V::Integer,0,1,1,{}}};
constexpr ParameterDescriptor sequence_acf_fft[]{
    {"acf_fft_size",C::Processing,V::Integer,0,65536,0,{}},
    {"acf_fft_length",C::Processing,V::Integer,0,1,1,{}}};
constexpr ParameterDescriptor correlation[]{
    {"spectrum_divisor",C::Processing,V::Number,1e-20,1e20,1,{}},
    {"spectral_exponent",C::Processing,V::Number,.000001,16,2,{}},
    {"inverse_scale",C::Processing,V::Number,1e-20,1e20,1,{}},
    {"correlation_normalization",C::Processing,V::Integer,0,2,0,{}},
    {"inverse_output",C::Processing,V::Integer,0,1,0,{}},
    {"transform",C::Processing,V::Integer,0,2,2,{}}};
constexpr ParameterDescriptor acf_size[]{
    {"samples",C::Processing,V::Integer,2,16384,512,{}},
    {"lag_count",C::Processing,V::Integer,2,16384,512,{}}};
constexpr ParameterDescriptor normalization[]{
    {"normalization",C::Processing,V::Integer,0,4,1,{}},
    {"norm_exponent",C::Processing,V::Number,.000001,32,2,{}}};
constexpr ParameterDescriptor sequence[]{
    {"method",C::Processing,V::Integer,0,2,0,{}},
    {"interpolation_kind",C::Processing,V::Integer,0,8,0,{}},
    {"spline_order",C::Processing,V::Integer,0,16,1,{}},
    {"fill_below",C::Processing,V::Number,-1e30,1e30,0,{}},
    {"fill_above",C::Processing,V::Number,-1e30,1e30,0,{}},
    {"extrapolate",C::Processing,V::Boolean,0,1,0,{}},
    {"bounds_error",C::Processing,V::Boolean,0,1,0,{}},
    {"keep_infinite_coordinates",C::Processing,V::Boolean,0,1,0,{}},
    {"source_linear",C::Processing,V::Boolean,0,1,0,{}}};
template<std::size_t A,std::size_t B>
constexpr auto join(const ParameterDescriptor (&a)[A],const ParameterDescriptor (&b)[B]) {
    std::array<ParameterDescriptor,A+B> result{};
    std::copy_n(a,A,result.begin());std::copy_n(b,B,result.begin()+A);return result;
}
template<std::size_t A,std::size_t B>
constexpr auto join(const std::array<ParameterDescriptor,A>& a,const ParameterDescriptor (&b)[B]) {
    std::array<ParameterDescriptor,A+B> result{};
    std::copy(a.begin(),a.end(),result.begin());std::copy_n(b,B,result.begin()+A);return result;
}
template<std::size_t N,std::size_t A>
constexpr auto prefix(const ParameterDescriptor (&a)[A]) {
    static_assert(N<=A);std::array<ParameterDescriptor,N> result{};
    std::copy_n(a,N,result.begin());return result;
}
template<std::size_t A,std::size_t B>
constexpr auto join(const ParameterDescriptor (&a)[A],const std::array<ParameterDescriptor,B>& b) {
    std::array<ParameterDescriptor,A+B> result{};
    std::copy_n(a,A,result.begin());std::copy(b.begin(),b.end(),result.begin()+A);return result;
}
constexpr auto frame_parameters=join(join(join(rates,windows),positions),padding);
constexpr auto fourier_parameters=join(join(join(join(rates,windows),positions),padding),fft);
constexpr auto acf_parameters=join(join(join(rates,acf_size),acf_fft),correlation);
constexpr auto acf_tempogram_parameters=join(join(join(join(join(rates,windows),frame_index),ramp),acf_fft),correlation);
constexpr auto normalized_acf_parameters=join(acf_tempogram_parameters,normalization);
constexpr auto sequence_windows=prefix<8>(windows);
constexpr auto sequence_parameters=join(join(join(join(join(join(rates,sequence_windows),sequence_ramp),sequence_acf_fft),correlation),normalization),sequence);

const ParameterDescriptor* descriptor(std::string_view kind,std::string_view key) noexcept {
    const auto descriptors=temporal_operation_parameters(kind);
    const auto found=std::find_if(descriptors.begin(),descriptors.end(),[&](const auto& value){return value.key==key;});
    return found==descriptors.end()?nullptr:&*found;
}
template<class T> bool number(const ResolvedProcessingNode& node,std::string_view key,T& result) noexcept {
    ProcessingParameterView value;
    if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);const auto* rule=descriptor(node.kind,key);
    if(!literal || !rule)return false;
    const auto n=std::visit([](auto v){return double(v);},*literal);
    if(!std::isfinite(n) || n<rule->minimum || n>rule->maximum || (rule->value_kind==V::Integer && std::floor(n)!=n))return false;
    result=T(n);return true;
}
bool flag(const ResolvedProcessingNode& node,std::string_view key,bool& result) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* p=std::get_if<bool>(&value);if(!p)return false;
    result=*p;return true;
}
bool window_coefficients(const ResolvedProcessingNode& n,TemporalOperationParameters& p) noexcept {
    ProcessingParameterView function,coefficients;
    if(resolve_processing_parameter(n,"function",function)!=Status::ok || resolve_processing_parameter(n,"coefficients",coefficients)!=Status::ok)return false;
    const auto* text=std::get_if<std::string_view>(&function);const auto* values=std::get_if<std::span<const float>>(&coefficients);
    if(!text || !parse_window_function(*text,p.function) || !values)return false;
    p.coefficients=*values;
    return (p.coefficients.empty() || p.coefficients.size()==p.window_length) &&
        std::all_of(p.coefficients.begin(),p.coefficients.end(),[](float x){return std::isfinite(x);});
}
bool window(const ResolvedProcessingNode& n,TemporalOperationParameters& p) noexcept {
    if(!number(n,"window_length",p.window_length) || !number(n,"hop",p.hop) || !number(n,"tail",p.tail) ||
       !flag(n,"center",p.center) || !flag(n,"periodic",p.periodic) || !flag(n,"odd_center_ceil",p.odd_center_ceil))return false;
    if(n.kind!="TempogramSequence" && !flag(n,"empty_centered_frame",p.empty_centered_frame))return false;
    return window_coefficients(n,p) && !(p.center && p.tail==1) && !(!p.center && p.tail==2);
}
bool position(const ResolvedProcessingNode& n,TemporalOperationParameters& p) noexcept {
    return number(n,"frame_index",p.frame_index) && number(n,"first_sample",p.first_sample) && flag(n,"use_first_sample",p.use_first_sample);
}
bool correlation_settings(const ResolvedProcessingNode& n,TemporalOperationParameters& p) noexcept {
    const bool sequence=n.kind=="TempogramSequence";
    return number(n,sequence?"acf_fft_size":"fft_size",p.fft_size) && number(n,sequence?"acf_fft_length":"fft_length",p.fft_length) &&
        number(n,"spectrum_divisor",p.spectrum_divisor) && number(n,"spectral_exponent",p.spectral_exponent) && number(n,"inverse_scale",p.inverse_scale) &&
        number(n,"correlation_normalization",p.correlation_normalization) && number(n,"inverse_output",p.inverse_output) && number(n,"transform",p.transform);
}
bool interpolation_settings(const ResolvedProcessingNode& n,TemporalOperationParameters& p) noexcept {
    return number(n,"method",p.method) && number(n,"interpolation_kind",p.interpolation_kind) && number(n,"spline_order",p.spline_order) &&
        number(n,"fill_below",p.fill_below) && number(n,"fill_above",p.fill_above) && flag(n,"extrapolate",p.extrapolate) &&
        flag(n,"bounds_error",p.bounds_error) && flag(n,"keep_infinite_coordinates",p.keep_infinite_coordinates) && flag(n,"source_linear",p.source_linear);
}
bool recipe(const ResolvedProcessingNode& n,TemporalOperationParameters& p) noexcept {
    if(n.kind=="SpectralAutocorrelation")return number(n,"samples",p.samples) && number(n,"lag_count",p.lag_count) && p.lag_count<=p.samples && correlation_settings(n,p);
    if(!window(n,p) || !number(n,n.kind=="TempogramSequence"?"acf_padding":"padding",p.padding))return false;
    if(n.kind=="TempogramSequence")return correlation_settings(n,p) && interpolation_settings(n,p);
    if(n.kind=="AutocorrelationTempogram")return number(n,"frame_index",p.frame_index) && correlation_settings(n,p);
    if(!position(n,p))return false;
    if(n.kind=="FourierTempogram")return number(n,"fft_size",p.fft_size) && number(n,"rotation",p.rotation) && flag(n,"zero_phase",p.zero_phase);
    return n.kind=="TemporalFrame" || correlation_settings(n,p);
}
bool normalized_recipe(const ResolvedProcessingNode& node,TemporalOperationParameters& p) noexcept {
    if(node.kind!="AutocorrelationTempogram" && node.kind!="TempogramSequence")return true;
    return number(node,"normalization",p.normalization) && number(node,"norm_exponent",p.norm_exponent);
}
bool valid_fft_settings(std::string_view kind,const TemporalOperationParameters& p) noexcept {
    if(kind=="TemporalFrame")return true;
    if(kind=="FourierTempogram")return !p.zero_phase || !p.fft_size || p.fft_size>=p.window_length-p.window_length/2;
    const auto count=kind=="SpectralAutocorrelation"?p.samples:p.window_length;
    return !p.fft_size || p.fft_size>=2*count-1;
}
NumericContract output_base(const NumericContract& input,Rank rank,std::size_t rows,std::size_t columns,Axis row_axis,Axis column_axis) noexcept {
    auto c=input;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;c.quantity=Quantity::Unspecified;c.unit=NumericUnit::Values;
    c.representation={};c.representation.alignment=alignof(float);c.dtype=DType::Float32;
    c.shape={rank,{row_axis,column_axis},{ExtentBounds{0,rows},ExtentBounds{columns,columns}}};
    c.coordinates={};return c;
}
void axis(NumericContract& c,std::size_t dimension,NumericUnit unit) noexcept {
    c.coordinates[dimension]={CoordinateMode::ExplicitFloat64LE,unit,{0,1},{0,1}};
}
NumericContract vector(const NumericContract& input,std::size_t size,Axis kind,NumericUnit unit) noexcept {
    auto c=output_base(input,Rank::Vector,size,1,kind,Axis::None);axis(c,0,unit);return c;
}
NumericContract complex(const NumericContract& input,std::size_t size,Axis kind,NumericUnit unit) noexcept {
    auto c=output_base(input,Rank::Matrix,size,2,kind,Axis::Component);axis(c,0,unit);
    c.coordinates[1]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};return c;
}
NumericContract frame_contract(const NumericContract& input,const TemporalOperationParameters& p) noexcept {
    return vector(input,p.window_length,Axis::Time,NumericUnit::Seconds);
}
NumericContract acf_contract(const NumericContract& input,const TemporalOperationParameters& p,PortId port) noexcept {
    const auto fft=temporal_fft_bound(p,"SpectralAutocorrelation");
    if(port==0 || port==5)return vector(input,port?p.lag_count-1:p.lag_count,Axis::Time,NumericUnit::Seconds);
    if(port==1)return complex(input,p.transform==2?fft/2+1:fft,Axis::Bin,NumericUnit::Hertz);
    if(port==2)return vector(input,fft,Axis::Bin,NumericUnit::Hertz);
    if(port==3)return complex(input,fft,Axis::Time,NumericUnit::Seconds);
    return vector(input,fft,Axis::Time,NumericUnit::Seconds);
}
NumericContract fourier_contract(const NumericContract& input,const TemporalOperationParameters& p,PortId port) noexcept {
    const auto fft=p.fft_size?p.fft_size:p.window_length;
    if(port==0)return complex(input,fft/2+1,Axis::Bin,NumericUnit::Hertz);
    if(port==3)return vector(input,fft/2+1,Axis::Bin,NumericUnit::Hertz);
    return vector(input,port==1?p.window_length:fft,Axis::Time,NumericUnit::Seconds);
}
std::size_t sequence_frames(const TemporalOperationParameters& p,std::size_t count) noexcept {
    const auto frames=temporal_frame_count(p,count);
    return p.method==1?frames:std::min(count,frames);
}
NumericContract sequence_vector_contract(const NumericContract& input,const TemporalOperationParameters& p,std::size_t bins,std::size_t columns) noexcept {
    return vector(input,p.method?bins:columns-1,p.method?Axis::Bin:Axis::Time,p.method?NumericUnit::Hertz:NumericUnit::Seconds);
}

NumericContract sequence_contract(const NumericContract& input,const TemporalOperationParameters& p,PortId port) noexcept {
    const auto bins=p.window_length/2+1,columns=p.method?p.window_length/2+1:p.window_length;
    if(port==7)return sequence_vector_contract(input,p,bins,columns);
    const auto width=port==0?columns:(port==3 || port==4)?bins:p.window_length;
    const bool frequency=(port==0 && p.method) || port==3 || port==4;
    auto c=output_base(input,Rank::Matrix,sequence_frames(p,std::size_t(input.shape.extents[0].maximum)),width,Axis::Time,frequency?Axis::Bin:Axis::Time);
    axis(c,0,NumericUnit::Seconds);axis(c,1,frequency?NumericUnit::Hertz:NumericUnit::Seconds);return c;
}
NumericContract result_contract(const NumericContract& input,const TemporalOperationParameters& p,std::string_view kind,PortId port) noexcept {
    if(kind=="TemporalFrame")return frame_contract(input,p);
    if(kind=="SpectralAutocorrelation")return acf_contract(input,p,port);
    if(kind=="FourierTempogram")return fourier_contract(input,p,port);
    if(kind=="TempogramSequence")return sequence_contract(input,p,port);
    return vector(input,port==3?p.window_length-1:p.window_length,Axis::Time,NumericUnit::Seconds);
}
bool close_coordinate(double a,double b) noexcept {
    return std::abs(a-b)<=8*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(a),std::abs(b)});
}
bool regular_coordinates(const AxisCoordinates& axis,const TemporalOperationParameters& p) noexcept {
    if(axis.mode!=CoordinateMode::Regular)return true;
    if(!axis.origin.denominator || !axis.step.denominator)return false;
    const bool seconds=axis.unit==NumericUnit::Seconds;
    return close_coordinate(double(axis.origin.numerator)/double(axis.origin.denominator),seconds?p.input_origin_s:0) &&
        close_coordinate(double(axis.step.numerator)/double(axis.step.denominator),seconds?1/p.input_rate:1);
}
bool native_input(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.shape.rank==Rank::Vector && c.channel_order.size()==1 &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        !c.representation.valid_bits && c.representation.normalization==Normalization::None;
}
Status make_outputs(const NumericContract& input,const TemporalOperationParameters& p,std::string_view kind,std::vector<OwnedProcessingContract>& output) {
    std::vector<OwnedProcessingContract> result(temporal_operation_output_count(kind));
    for(PortId port=0;port<result.size();++port){
        const auto c=result_contract(input,p,kind,port);
        if(!c.shape.extents[0].maximum)return Status::incompatible_contract;
        const auto status=own_processing_contract(c,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,result[port]);
        if(status!=Status::ok)return status;
    }
    output=std::move(result);return Status::ok;
}
}
bool temporal_operation_kind(std::string_view kind) noexcept {return temporal_operation_output_count(kind)!=0;}
std::size_t temporal_operation_output_count(std::string_view kind) noexcept {
    if(kind=="TemporalFrame")return 2;
    if(kind=="SpectralAutocorrelation")return 6;
    if(kind=="FourierTempogram" || kind=="AutocorrelationTempogram")return 4;
    return kind=="TempogramSequence"?8:0;
}
std::span<const ParameterDescriptor> temporal_operation_parameters(std::string_view kind) noexcept {
    if(kind=="TemporalFrame")return frame_parameters;
    if(kind=="SpectralAutocorrelation")return acf_parameters;
    if(kind=="FourierTempogram")return fourier_parameters;
    if(kind=="AutocorrelationTempogram")return normalized_acf_parameters;
    if(kind=="TempogramSequence")return sequence_parameters;
    return {};
}
bool temporal_operation_settings(const ResolvedProcessingNode& node,TemporalOperationParameters& output) noexcept {
    if(!temporal_operation_kind(node.kind))return false;
    TemporalOperationParameters p;
    if(!number(node,"input_rate",p.input_rate) || !number(node,"input_origin_s",p.input_origin_s) || !recipe(node,p) ||
       !normalized_recipe(node,p) || !valid_fft_settings(node.kind,p))return false;
    // Sequence deliberately uses the source constant Fourier padding; padding
    // config belongs to its independent ACF branch.
    if(node.kind=="TempogramSequence")p.empty_centered_frame=true;
    double fs=0;std::size_t hop=0;
    if(!number(node,"sample_rate",fs) || !number(node,"hop_samples",hop) || !hop)return false;
    if(fs)p.input_rate=fs/double(hop);
    output=p;return true;
}
std::size_t temporal_frame_count(const TemporalOperationParameters& p,std::size_t count) noexcept {
    if(p.window_length<2 || p.window_length>16384 || !p.hop || p.hop>65536 || p.tail>3 || count>65536)return 0;
    if(!count)return p.empty_centered_frame && p.center && p.tail==0 && p.window_length%2==0?1:0;
    const auto offset=p.window_length/2+std::size_t(p.odd_center_ceil && p.window_length%2);
    if(p.tail==3)return 1+(count-1)/p.hop;
    if(p.tail==1)return std::min(count<=p.window_length?1:2+(count-p.window_length-1)/p.hop,1+(count-1)/p.hop);
    if(p.tail==2){const auto terminal=count+offset-p.window_length/2;return std::min(1+terminal/p.hop+std::size_t(terminal%p.hop!=0),1+(count+offset-1)/p.hop);}
    const auto padded=count+(p.center?offset+p.window_length/2:0);
    return padded<p.window_length?0:1+(padded-p.window_length)/p.hop;
}
std::size_t temporal_fft_bound(const TemporalOperationParameters& p,std::string_view kind) noexcept {
    if(p.fft_size)return p.fft_size<=65536?p.fft_size:0;
    if(kind=="FourierTempogram")return p.window_length;
    const auto samples=kind=="SpectralAutocorrelation"?p.samples:p.window_length;
    if(samples<2 || samples>16384)return 0;
    std::size_t maximum=1;while(maximum<2*samples)maximum*=2;return maximum;
}
bool temporal_input_contract(const NumericContract& c,const TemporalOperationParameters& p,std::string_view kind) noexcept {
    if(validate_contract(DataContract{c},FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX})!=Status::ok)return false;
    if(!native_input(c))return false;
    if(!c.shape.extents[0].maximum || c.shape.extents[0].maximum>65536 || c.shape.extents[0].minimum>c.shape.extents[0].maximum)return false;
    if(kind=="SpectralAutocorrelation" && c.shape.extents[0]!=ExtentBounds{p.samples,p.samples})return false;
    const auto& axis=c.coordinates[0];
    return (axis.mode==CoordinateMode::None || axis.unit==NumericUnit::Seconds || axis.unit==NumericUnit::Values || axis.unit==NumericUnit::Frames) && regular_coordinates(axis,p);
}
Status temporal_operation_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,std::vector<OwnedProcessingContract>& output) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    TemporalOperationParameters p;
    if(!temporal_operation_settings(resolved,p) || !temporal_input_contract(input,p,node.kind))return Status::incompatible_contract;
#if defined(__cpp_exceptions)
    try {
#endif
        return make_outputs(input,p,node.kind,output);
#if defined(__cpp_exceptions)
    } catch(const std::bad_alloc&){return Status::exhausted;}catch(const std::length_error&){return Status::exhausted;}
#endif
}
}
