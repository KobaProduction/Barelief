#include <domain/music/contracts.h>
#include <domain/graph/admission.h>
#include <signal_contracts/resampling_policy.h>
#include <signal_contracts/spectral_calibration.h>
#include <algorithm>
#include <cmath>
#include <limits>
namespace domain {
using namespace signal_contracts;
namespace {
ResolvedProcessingNode view(const ConfiguredProcessingNode& n) {
    return {n.id,n.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,n.inputs,n.outputs,&n.parameters,n.enabled};
}
bool number(const ConfiguredProcessingNode& n,std::string_view key,double& result) {
    ProcessingParameterView value;
    if(resolve_processing_parameter(view(n),key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
    result=std::visit([](auto v){return static_cast<double>(v);},*literal);return std::isfinite(result);
}
bool flag(const ConfiguredProcessingNode& n,std::string_view key,bool& result) {
    ProcessingParameterView value;
    if(resolve_processing_parameter(view(n),key,value)!=Status::ok)return false;
    const auto* boolean=std::get_if<bool>(&value);if(!boolean)return false;
    result=*boolean;return true;
}
std::string_view text(const ConfiguredProcessingNode& n,std::string_view key) {
    ProcessingParameterView value;
    if(resolve_processing_parameter(view(n),key,value)!=Status::ok)return {};
    const auto* result=std::get_if<std::string_view>(&value);return result?*result:std::string_view{};
}
bool valid_coefficient_count(const ConfiguredProcessingNode& n,std::size_t count) {
    ProcessingParameterView value;
    if(resolve_processing_parameter(view(n),"gains",value)!=Status::ok)return false;
    const auto* gains=std::get_if<std::span<const float>>(&value);
    return gains && gains->size()==count && std::all_of(gains->begin(),gains->end(),[](float v){return valid_profile_gain(v);});
}
bool grid_matches(const NumericContract& input,std::uint64_t fs,std::uint64_t fft,std::uint64_t first) {
    const auto& grid=input.coordinates[0];
    if(grid.mode==CoordinateMode::None)return true; // Explicit Fs/N supplies coordinates.
    if(grid.mode!=CoordinateMode::Regular || grid.unit!=NumericUnit::Hertz)return false;
    if(grid.step.numerator<=0 || grid.origin.numerator<0 || !grid.step.denominator || !grid.origin.denominator)return false;
    const long double step=static_cast<long double>(grid.step.numerator)/grid.step.denominator;
    const long double origin=static_cast<long double>(grid.origin.numerator)/grid.origin.denominator;
    const long double expected=static_cast<long double>(fs)/fft;
    return std::abs(step-expected)<=std::numeric_limits<double>::epsilon()*expected &&
        std::abs(origin-first*expected)<=std::numeric_limits<double>::epsilon()*std::max(1.L,first*expected);
}
bool amplitude_representation_matches(std::string_view representation,Quantity quantity) {
    if(representation=="magnitude")return quantity==Quantity::SpectrumMagnitude;
    if(representation=="peak_amplitude")return quantity==Quantity::SpectrumPeakAmplitude;
    if(representation=="rms_amplitude")return quantity==Quantity::SpectrumRmsAmplitude;
    return representation=="window_scaled_magnitude" && quantity==Quantity::SpectrumWindowScaledMagnitude;
}
bool representation_matches(const ConfiguredProcessingNode& n,const NumericContract& input) {
    const auto representation=text(n,"representation");
    if(input.unit==NumericUnit::RawAmplitude)return amplitude_representation_matches(representation,input.quantity);
    if(input.unit==NumericUnit::SquaredAmplitude)return
        (representation=="power" && input.quantity==Quantity::SpectrumPower) ||
        (representation=="bin_power" && input.quantity==Quantity::SpectrumBinPower);
    return representation=="psd" && input.quantity==Quantity::SpectrumPsd && input.unit==NumericUnit::SquaredAmplitudePerHertz;
}
bool profile_spectrum_geometry(const NumericContract& input,double fft,double fs,double first,bool half,bool dc,bool nyquist) {
    const auto size=static_cast<std::uint64_t>(fft);if(size&(size-1))return false;
    const auto count=half?size/2+std::uint64_t(dc)-std::uint64_t(!nyquist):size-1+std::uint64_t(dc);
    if(first!=(dc?0:1) || input.shape.extents[0]!=ExtentBounds{count,count})return false;
    return (!input.clock.sample_rate || input.clock.sample_rate==std::uint64_t(fs)) && grid_matches(input,std::uint64_t(fs),size,std::uint64_t(first));
}
bool profile_geometry(const ConfiguredProcessingNode& n,const NumericContract& input) {
    double fft=0,fs=0,first=0;bool half=false,dc=false,nyquist=false;
    if(!number(n,"fft_size",fft) || !number(n,"sample_rate",fs) || !number(n,"first_bin",first))return false;
    if(!flag(n,"positive_half",half) || !flag(n,"include_dc",dc) || !flag(n,"include_nyquist",nyquist))return false;
    if(fft<2 || fft>UINT32_MAX || std::floor(fft)!=fft || fs<=0 || fs>UINT32_MAX || std::floor(fs)!=fs)return false;
    if(first<0 || std::floor(first)!=first || first>fft)return false;
    return profile_spectrum_geometry(input,fft,fs,first,half,dc,nyquist);
}
}
namespace {
ProcessingDiagnostic profile_issue(const ConfiguredProcessingNode& node,ProcessingReason reason,std::string_view parameter,
                                   ProcessingError error=ProcessingError::FrontendMismatch) noexcept {
    return {error,node.id,0,{},reason,parameter};
}
bool normalizer_input_supported(const NumericContract& input) noexcept {
    return input.dtype==DType::Float32 && input.shape.rank==Rank::Vector && input.shape.axes[0]==Axis::Bin &&
        input.channel_order.size()==1 && input.representation.encoding==StorageEncoding::Native &&
        input.representation.normalization==Normalization::None && input.shape.extents[0].maximum &&
        input.shape.extents[0].minimum==input.shape.extents[0].maximum;
}
ProcessingDiagnostic profile_frontend_diagnostic(const ConfiguredProcessingNode& node) noexcept {
    bool declared=false;double quality=0;
    if(!flag(node,"frontend_declared",declared) || !declared)return profile_issue(node,ProcessingReason::Frontend,"frontend_declared");
    if(text(node,"channel_policy")!="mean" && text(node,"channel_policy")!="select")return profile_issue(node,ProcessingReason::Frontend,"channel_policy");
    ResamplerMode mode;
    if(!parse_resampler_mode(text(node,"resampler_mode"),mode))return profile_issue(node,ProcessingReason::Resampler,"resampler_mode");
    if(!number(node,"resampler_quality",quality) || quality<0 || quality>10 || std::floor(quality)!=quality)return profile_issue(node,ProcessingReason::Resampler,"resampler_quality");
    return {};
}
ProcessingDiagnostic profile_geometry_diagnostic(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    double fft=0,fs=0;
    if(!number(node,"fft_size",fft) || fft<2 || fft>UINT32_MAX || std::floor(fft)!=fft)return profile_issue(node,ProcessingReason::FftSize,"fft_size");
    const auto size=std::uint64_t(fft);
    if(size&(size-1))return profile_issue(node,ProcessingReason::FftSize,"fft_size");
    if(!number(node,"sample_rate",fs) || fs<1 || fs>UINT32_MAX || std::floor(fs)!=fs ||
       (input.clock.sample_rate && input.clock.sample_rate!=std::uint64_t(fs)))return profile_issue(node,ProcessingReason::SampleRate,"sample_rate");
    if(!profile_geometry(node,input))return profile_issue(node,ProcessingReason::SpectrumLayout,"first_bin");
    return {};
}
bool profile_number_in_range(const ConfiguredProcessingNode& node,std::string_view key,double& value) noexcept {
    const auto* descriptor=parameter_descriptor(NodeKind::FrequencyNormalizer,key);
    return descriptor && number(node,key,value) && value>=descriptor->minimum && value<=descriptor->maximum;
}

ProcessingDiagnostic profile_window_diagnostic(const ConfiguredProcessingNode& node) noexcept {
    WindowFunction window;
    if(!parse_window_function(text(node,"window_function"),window))return profile_issue(node,ProcessingReason::WindowFunction,"window_function");
    double coherent=0,power=0;
    if(!profile_number_in_range(node,"coherent_gain",coherent))return profile_issue(node,ProcessingReason::CoherentGain,"coherent_gain");
    if(!profile_number_in_range(node,"power_gain",power))return profile_issue(node,ProcessingReason::PowerGain,"power_gain");
    return {};
}
ProcessingDiagnostic profile_gains_diagnostic(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    ProcessingParameterView value;
    if(resolve_processing_parameter(view(node),"gains",value)!=Status::ok)return profile_issue(node,ProcessingReason::GainValue,"gains",ProcessingError::InvalidParameters);
    const auto* gains=std::get_if<std::span<const float>>(&value);
    if(!gains)return profile_issue(node,ProcessingReason::GainValue,"gains",ProcessingError::InvalidParameters);
    if(gains->size()!=input.shape.extents[0].maximum)return profile_issue(node,ProcessingReason::GainCount,"gains",ProcessingError::InvalidParameters);
    if(!valid_coefficient_count(node,gains->size()))return profile_issue(node,ProcessingReason::GainValue,"gains",ProcessingError::InvalidParameters);
    return {};
}
}
namespace {
ProcessingDiagnostic configured_profile_diagnostic(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    auto diagnostic=profile_frontend_diagnostic(node);if(!diagnostic)return diagnostic;
    diagnostic=profile_geometry_diagnostic(node,input);if(!diagnostic)return diagnostic;
    diagnostic=profile_window_diagnostic(node);if(!diagnostic)return diagnostic;
    if(!representation_matches(node,input))return profile_issue(node,ProcessingReason::SpectrumRepresentation,"representation");
    return profile_gains_diagnostic(node,input);
}
}
ProcessingDiagnostic frequency_normalizer_diagnostic(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    const auto mode=text(node,"mode");double gain=0;
    if(mode!="off" && mode!="manual" && mode!="profile")return profile_issue(node,ProcessingReason::Frontend,"mode",ProcessingError::InvalidParameters);
    if(!number(node,"manual_gain",gain) || gain<0)return profile_issue(node,ProcessingReason::GainValue,"manual_gain",ProcessingError::InvalidParameters);
    if(!normalizer_input_supported(input))return profile_issue(node,ProcessingReason::SpectrumLayout,"input");
    if(mode!="profile")return {};
    return configured_profile_diagnostic(node,input);
}

Status validate_frequency_normalizer(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    const auto diagnostic=frequency_normalizer_diagnostic(node,input);
    if(diagnostic)return Status::ok;
    return diagnostic.error==ProcessingError::InvalidParameters?Status::invalid_argument:Status::incompatible_contract;
}
}
