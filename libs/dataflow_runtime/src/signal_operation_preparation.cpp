#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
dsp_kernels::WindowFunction window_kind(std::string_view text) {
    auto result=dsp_kernels::WindowFunction::Hann;(void)dsp_kernels::parse_window_function(text,result);return result;
}
dsp_kernels::SpectrumProjection projection_kind(std::string_view text) {
    auto result=dsp_kernels::SpectrumProjection::Magnitude;(void)dsp_kernels::parse_spectrum_projection(text,result);return result;
}
}
namespace {
Status prepare_polar(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n) {
    if(n.kind=="ComplexPolar")return kernel.spectrum_scaling.prepare(1,1,1,{dsp_kernels::SpectrumProjection::Magnitude,false})?Status::ok:Status::invalid_argument;
    double coherent=0,power=0,fs=0;ProcessingParameterView positive,normalized;
    if(!numeric_parameter(n,"coherent_gain",coherent) || !numeric_parameter(n,"power_gain",power) || !numeric_parameter(n,"sample_rate",fs))return Status::invalid_argument;
    if(resolve_processing_parameter(n,"positive_half",positive)!=Status::ok || !std::holds_alternative<bool>(positive) ||
       resolve_processing_parameter(n,"normalize",normalized)!=Status::ok || !std::holds_alternative<bool>(normalized))return Status::invalid_argument;
    node.half_last=std::get<bool>(positive);
    return kernel.spectrum_scaling.prepare(fs,coherent,power,{projection_kind(operation_text(n,"representation")),std::get<bool>(normalized)})?Status::ok:Status::invalid_argument;
}
Status prepare_paired(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n) {
    const auto samples=kernel.input.size();kernel.imaginary.resize(samples);
    kernel.output.resize(n.kind=="RealFourier"?samples+2:samples*2);
    if(n.kind=="ComplexPolar" || n.kind=="AmplitudePhase")return prepare_polar(kernel,node,n);
    if(n.kind=="Fourier"){
        kernel.operation.emplace<dsp_kernels::FFT>();
        return std::get<dsp_kernels::FFT>(kernel.operation).prepare(samples,operation_text(n,"radix")=="radix4")?Status::ok:Status::unavailable;
    }
    kernel.operation.emplace<dsp_kernels::PreparedRfft>();
    return std::get<dsp_kernels::PreparedRfft>(kernel.operation).prepare(samples,operation_text(n,"radix")=="radix4")?Status::ok:Status::unavailable;
}
Status prepare_rfft(OperationKernelState& kernel,const ResolvedProcessingNode& n) {

        kernel.operation.emplace<dsp_kernels::PreparedRfft>();
        if(!std::get<dsp_kernels::PreparedRfft>(kernel.operation).prepare(kernel.input.size(),operation_text(n,"radix")=="radix4"))return Status::unavailable;
        return Status::ok;
}
Status prepare_projection(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& source) {
    double fft_size=0,sample_rate=0;
    if(!numeric_parameter(source,"fft_size",fft_size) ||
       !numeric_parameter(source,"sample_rate",sample_rate))return Status::invalid_argument;
    dsp_kernels::WindowFunction window;
    dsp_kernels::SpectrumProjection representation;
    if(!dsp_kernels::parse_window_function(operation_text(source,"window_function"),window) ||
       !dsp_kernels::parse_spectrum_projection(operation_text(source,"representation"),representation))
        return Status::invalid_argument;
    dsp_kernels::PreparedWindows windows{std::size_t(fft_size)};
    const auto scaling=dsp_kernels::window_scaling(windows.coefficients(window));
    if(!scaling)return Status::invalid_argument;
    node.window_function=window;
    kernel.operation.emplace<dsp_kernels::PreparedSpectrumProjection>();
    const dsp_kernels::SpectrumProjectionOptions options{representation,
        representation!=dsp_kernels::SpectrumProjection::Magnitude};
    return std::get<dsp_kernels::PreparedSpectrumProjection>(kernel.operation).prepare(
        std::size_t(fft_size),sample_rate,*scaling,options)?Status::ok:Status::invalid_argument;
}
Status prepare_normalizer_profile(OperationKernelState& kernel,const ResolvedProcessingNode& n) {
    ProcessingParameterView values;
    if(resolve_processing_parameter(n,"gains",values)!=Status::ok)return Status::invalid_argument;
    const auto* gains=std::get_if<std::span<const float>>(&values);
    auto& normalizer=std::get<dsp_kernels::PreparedFrequencyNormalizer>(kernel.operation);
    if(!gains || gains->size()!=kernel.input.size() || !normalizer.prepare(*gains))return Status::incompatible_contract;
    return Status::ok;
}
Status prepare_normalizer(OperationKernelState& kernel,const ResolvedProcessingNode& n) {
    const auto mode=operation_text(n,"mode");
    kernel.operation.emplace<dsp_kernels::PreparedFrequencyNormalizer>();
    if(mode=="profile")return prepare_normalizer_profile(kernel,n);
    auto& normalizer=std::get<dsp_kernels::PreparedFrequencyNormalizer>(kernel.operation);
    double gain=1;
    if(mode=="manual" && !numeric_parameter(n,"manual_gain",gain))return Status::invalid_argument;
    return normalizer.prepare_manual(kernel.input.size(),float(gain))?Status::ok:Status::invalid_argument;
}
}
Status prepare_signal_operation(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n) {
    if(paired_processing(n.operation))return prepare_paired(kernel,node,n);
    if(n.kind=="RFFT")return prepare_rfft(kernel,n);
    if(n.kind=="SpectrumProjection")return prepare_projection(kernel,node,n);
    if(n.kind=="FrequencyNormalizer")return prepare_normalizer(kernel,n);
    return Status::unavailable;
}
}
