#include <domain/signal/contracts.h>
#include <domain/numeric/contracts.h>
#include <dsp_kernels/window_math.h>
#include <cmath>
namespace domain {
using namespace signal_contracts;
namespace {
bool number(const ConfiguredProcessingNode& node,std::string_view key,std::uint64_t& out) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(view,key,field)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&field);if(!literal)return false;
    const auto value=std::visit([](auto v){return static_cast<long double>(v);},*literal);
    if(!std::isfinite(value) || value<0 || value>UINT32_MAX || std::floor(value)!=value)return false;
    out=static_cast<std::uint64_t>(value);return true;
}
bool scale_frequency_ratio(std::uint64_t& numerator,int exponent,std::uint64_t& denominator) {
    denominator=1;
    if(exponent<0){if(exponent<=-64)return false;denominator<<=-exponent;}
    else {if(exponent>=63 || numerator>(std::uint64_t(INT64_MAX)>>exponent))return false;numerator<<=exponent;}
    return true;
}
bool frequency_step_ratio(double step,Ratio& out) {
    int exponent=0;const auto fraction=std::frexp(step,&exponent);
    std::uint64_t numerator=std::uint64_t(std::ldexp(fraction,53));exponent-=53;
    while(exponent<0 && !(numerator&1)){numerator>>=1;++exponent;}
    std::uint64_t denominator=1;
    if(!scale_frequency_ratio(numerator,exponent,denominator))return false;
    if(numerator>std::uint64_t(INT64_MAX))return false;
    out={std::int64_t(numerator),denominator};return true;
}

std::string_view text(const ConfiguredProcessingNode& node,std::string_view key) {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(view,key,field)!=Status::ok)return {};
    const auto* value=std::get_if<std::string_view>(&field);return value?*value:std::string_view{};
}
}
bool declared_frequency_axis_matches(const AxisCoordinates& axis,double step,std::uint64_t first) {
    if(axis.unit!=NumericUnit::Hertz || axis.mode==CoordinateMode::None)return true;
    if(axis.mode!=CoordinateMode::Regular || !axis.origin.denominator || !axis.step.denominator)return false;
    const auto actual_step=double(axis.step.numerator)/axis.step.denominator;
    const auto actual_origin=double(axis.origin.numerator)/axis.origin.denominator;
    const auto tolerance=1e-12*step;
    return std::abs(actual_step-step)<=tolerance &&
        std::abs(actual_origin-double(first)*step)<=tolerance*(double(first)+1);
}
namespace {
NumericContract peak_scalar_contract(const NumericContract& input) {
    auto scalar=input;
    scalar.shape={};scalar.coordinates={};scalar.semantic=Semantic::Scalar;
    scalar.quantity=Quantity::Unspecified;scalar.unit=NumericUnit::Hertz;
    scalar.representation={};scalar.representation.alignment=alignof(float);
    return scalar;
}
}
Status spectrum_peak_output_contracts(const NumericContract& input,std::array<OwnedProcessingContract,2>& output) noexcept {
    if(input.dtype!=DType::Float32 || input.shape.rank!=Rank::Vector || input.channel_order.size()!=1 ||
       input.representation.encoding!=StorageEncoding::Native || input.representation.normalization!=Normalization::None)return Status::incompatible_contract;
    auto scalar=peak_scalar_contract(input);
    std::array<OwnedProcessingContract,2> candidate;
    const FrameLimits limits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
    auto status=own_processing_contract(scalar,limits,SIZE_MAX,candidate[0]);
    if(status!=Status::ok)return status;
    scalar.quantity=input.quantity;scalar.unit=input.unit;
    status=own_processing_contract(scalar,limits,SIZE_MAX,candidate[1]);
    if(status==Status::ok)output=std::move(candidate);
    return status;
}

Status window_gain_output_contract(const NumericContract& input,OwnedProcessingContract& out) noexcept {
    auto gain=input;gain.dtype=DType::Float64;gain.shape={};gain.coordinates={};gain.semantic=Semantic::Scalar;gain.quantity=Quantity::Unspecified;gain.unit=NumericUnit::Values;gain.representation={};gain.representation.alignment=alignof(double);
    return own_processing_contract(gain,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
double processing_window_gain(const ConfiguredProcessingNode& node,std::size_t count,bool power) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;if(resolve_processing_parameter(resolved,"function",field)!=Status::ok || !std::holds_alternative<std::string_view>(field))return 0;
    const auto value=std::get<std::string_view>(field);
    auto function=dsp_kernels::WindowFunction::Rectangular;
    if(node.enabled && !dsp_kernels::parse_window_function(value,function))return 0;
    return power?dsp_kernels::window_square_sum(count,function):dsp_kernels::window_sum(count,function);
}
Status spectrum_amplitude_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    std::uint64_t size=0;if(!number(node,"fft_size",size) || size<2 || input.dtype!=DType::Float32 || input.shape.rank!=Rank::Vector || input.quantity!=Quantity::SpectrumMagnitude)return Status::invalid_argument;
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView value;if(resolve_processing_parameter(resolved,"positive_half",value)!=Status::ok || !std::holds_alternative<bool>(value))return Status::invalid_argument;
    const auto count=std::get<bool>(value)?size/2:size-1;
    if(input.shape.extents[0]!=ExtentBounds{count,count})return Status::incompatible_contract;
    auto output=input;output.semantic=Semantic::SpectrumMagnitude;output.quantity=Quantity::SpectrumPeakAmplitude;
    const FrameLimits frame{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};return own_processing_contract(output,frame,SIZE_MAX,out);
}
namespace {
Status amplitude_phase_parameters(const ConfiguredProcessingNode& node,const NumericContract& input,
    std::uint64_t& size,bool& normalize) {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ComplexPolar,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView half,normalized;
    if(resolve_processing_parameter(resolved,"positive_half",half)!=Status::ok || !std::holds_alternative<bool>(half) ||
       resolve_processing_parameter(resolved,"normalize",normalized)!=Status::ok || !std::holds_alternative<bool>(normalized))return Status::invalid_argument;
    const auto extent=input.shape.extents[0];
    if(!extent.maximum || extent.minimum!=extent.maximum || extent.maximum>UINT32_MAX/2)return Status::incompatible_contract;
    // Both explicit layouts exclude DC: half keeps k=1..N/2, full k=1..N-1.
    size=std::get<bool>(half)?2*extent.maximum:extent.maximum+1;
    normalize=std::get<bool>(normalized);
    return Status::ok;
}
Quantity amplitude_quantity(std::string_view representation,bool normalize) {
    if(!normalize)return representation=="power" || representation=="psd" || representation=="bin_power"?Quantity::SpectrumPower:Quantity::SpectrumMagnitude;
    if(representation=="magnitude" || representation=="window_scaled_magnitude")return Quantity::SpectrumWindowScaledMagnitude;
    if(representation=="power")return Quantity::SpectrumPower;
    if(representation=="psd")return Quantity::SpectrumPsd;
    if(representation=="rms_amplitude")return Quantity::SpectrumRmsAmplitude;
    if(representation=="bin_power")return Quantity::SpectrumBinPower;
    return Quantity::SpectrumPeakAmplitude;
}
}
namespace {
Status own_amplitude_phase(NumericContract result,std::array<OwnedProcessingContract,2>& polar,
    std::string_view representation,bool normalize) {
    auto phase=std::get<NumericContract>(polar[1].view());
    phase.coordinates=result.coordinates;
    if(own_processing_contract(phase,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,polar[1])!=Status::ok)return Status::invalid_argument;
    result.unit=representation=="power" || representation=="bin_power" || (!normalize && representation=="psd")?NumericUnit::SquaredAmplitude:representation=="psd"?NumericUnit::SquaredAmplitudePerHertz:NumericUnit::RawAmplitude;
    if(own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,polar[0])!=Status::ok)return Status::invalid_argument;
    return Status::ok;
}
}
namespace {
Status configure_amplitude_phase(const ConfiguredProcessingNode& node,std::uint64_t size,bool normalize,std::array<OwnedProcessingContract,2>& polar) {
    auto result=std::get<NumericContract>(polar[0].view());const auto representation=text(node,"representation");
    result.quantity=amplitude_quantity(representation,normalize);
    if(representation!="magnitude" && representation!="window_scaled_magnitude" && representation!="peak_amplitude" && representation!="power" && representation!="psd" && representation!="rms_amplitude" && representation!="bin_power")return Status::invalid_argument;
    std::uint64_t fs=0;if(!number(node,"sample_rate",fs) || !fs)return Status::invalid_argument;
    result.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{std::int64_t(fs),size},{std::int64_t(fs),size}};
    if(own_amplitude_phase(result,polar,representation,normalize)!=Status::ok)return Status::invalid_argument;
    return Status::ok;
}
}
Status amplitude_phase_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,std::array<OwnedProcessingContract,2>& output) noexcept {
    std::array<OwnedProcessingContract,2> polar;auto status=complex_polar_output_contracts(input,polar);if(status!=Status::ok)return status;
    std::uint64_t size=0;bool normalize=false;
    status=amplitude_phase_parameters(node,input,size,normalize);if(status!=Status::ok)return status;
    status=configure_amplitude_phase(node,size,normalize,polar);if(status!=Status::ok)return status;
    output=std::move(polar);return Status::ok;
}
Status frequency_step_output_contract(const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.dtype!=DType::UInt32 || input.shape.rank!=Rank::Scalar || input.channel_order.size()!=1 || input.representation.encoding!=StorageEncoding::Native || input.representation.normalization!=Normalization::None)return Status::incompatible_contract;
    auto output=input;output.dtype=DType::Float64;output.semantic=Semantic::Scalar;output.quantity=Quantity::Unspecified;output.unit=NumericUnit::Hertz;output.coordinates={};output.representation={};output.representation.alignment=alignof(double);
    return own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
namespace {
Status own_frequency_grid(const NumericContract& input,double step,std::uint64_t first,OwnedProcessingContract& out) {
    Ratio ratio;
    if(!frequency_step_ratio(step,ratio) || (first && std::uint64_t(ratio.numerator)>std::uint64_t(INT64_MAX)/first))return Status::overflow;
    auto output=input;output.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{std::int64_t(first*std::uint64_t(ratio.numerator)),ratio.denominator},ratio};
    return own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
}
Status explicit_frequency_grid_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;std::uint64_t first=0;
    if(input.shape.rank!=Rank::Vector || !number(node,"first_bin",first) || resolve_processing_parameter(resolved,"bin_hz",parameter)!=Status::ok || !std::holds_alternative<NumericLiteral>(parameter))return Status::invalid_argument;
    const double step=std::visit([](auto v){return double(v);},std::get<NumericLiteral>(parameter));
    if(!std::isfinite(step) || step<=0 || step>4294967295.)return Status::invalid_argument;
    if(!declared_frequency_axis_matches(input.coordinates[0],step,first))return Status::incompatible_contract;
    return own_frequency_grid(input,step,first,out);
}

}
