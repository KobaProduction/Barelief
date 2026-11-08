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
bool matching_backend(const std::shared_ptr<const dsp_kernels::FourierBackendFactory>& factory,std::string_view requested) noexcept {
    if(!factory)return false;
    const auto* id=factory->id();return id && requested==id;
}
bool select_named_backend(const NumericRuntimeProfile& profile,std::string_view id,const dsp_kernels::FourierBackendFactory*& output) noexcept {
    if(matching_backend(profile.fourier_backend,id)){
        output=profile.fourier_backend.get();return true;
    }
    for(const auto& factory:profile.additional_fourier_backends){
        if(!matching_backend(factory,id))continue;
        output=factory.get();return true;
    }
    return false;
}
}
bool select_fourier_backend(const ResolvedProcessingNode& node,const NumericRuntimeProfile& profile,
    const dsp_kernels::FourierBackendFactory*& output) noexcept {
    output=nullptr;ProcessingParameterView value;
    if(resolve_processing_parameter(node,"backend_id",value)!=Status::ok)return false;
    const auto* id=std::get_if<std::string_view>(&value);if(!id)return false;
    if(id->empty())return true;
    return select_named_backend(profile,*id,output);
}
namespace {
bool flag(const ResolvedProcessingNode& node,std::string_view key,bool& result) {
    ProcessingParameterView value;if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* found=std::get_if<bool>(&value);if(!found)return false;result=*found;return true;
}
bool raw_options(const ResolvedProcessingNode& node,dsp_kernels::SpectrumProjectionOptions& result) {
    const auto norm=operation_text(node,"magnitude_norm"),zero=operation_text(node,"zero_phase");
    if((norm!="hypot" && norm!="squared_norm" && norm!="scaled_ratio_fma") || (zero!="zero" && zero!="atan2"))return false;
    result.magnitude_norm=norm=="hypot"?dsp_kernels::ComplexMagnitudeNorm::Hypot:
        norm=="squared_norm"?dsp_kernels::ComplexMagnitudeNorm::SquaredNorm:dsp_kernels::ComplexMagnitudeNorm::ScaledRatioFma;
    result.zero_phase=zero=="zero"?dsp_kernels::ZeroMagnitudePhase::Zero:dsp_kernels::ZeroMagnitudePhase::Atan2;
    return flag(node,"real_endpoints",result.real_endpoints);
}
bool projection_geometry(const ResolvedProcessingNode& source,std::size_t bins,dsp_kernels::SpectrumProjectionGeometry& geometry) {
    double size=0,first=0;
    if(!numeric_parameter(source,"fft_size",size) || (source.kind!="AmplitudePhase" && !numeric_parameter(source,"first_bin",first)) ||
       size<1 || size>UINT32_MAX || std::floor(size)!=size || first<0 || first>UINT32_MAX || std::floor(first)!=first ||
       !flag(source,"positive_half",geometry.one_sided))return false;
    geometry.fft_size=std::size_t(size);geometry.first_bin=std::size_t(first);geometry.bins=bins;
    const auto available=geometry.one_sided?geometry.fft_size/2+1:geometry.fft_size;
    return geometry.first_bin<available && bins<=available-geometry.first_bin;
}
Status prepare_polar(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& source) {
    const auto input=std::get<NumericContract>(node.numeric_input);
    const auto bins=std::size_t(input.shape.extents[input.shape.rank==Rank::Matrix?1:0].maximum);
    if(!raw_options(source,kernel.polar_options) || !projection_geometry(source,bins,kernel.polar_geometry))return Status::invalid_argument;
    kernel.polar_options.representation=dsp_kernels::SpectrumProjection::Magnitude;kernel.polar_options.compensate=false;
    if(source.kind=="ComplexPolar")return kernel.spectrum_scaling.prepare_raw(kernel.polar_options)?Status::ok:Status::invalid_argument;
    double fs=0;ProcessingParameterView coefficients;
    if(resolve_processing_parameter(source,"window_coefficients",coefficients)!=Status::ok || !std::holds_alternative<std::span<const float>>(coefficients) || !numeric_parameter(source,"sample_rate",fs) ||
       !flag(source,"normalize",kernel.polar_options.compensate))return Status::invalid_argument;
    if(!dsp_kernels::parse_spectrum_projection(operation_text(source,"representation"),kernel.polar_options.representation))
        return Status::invalid_argument;
    kernel.bin_hz=fs/double(kernel.polar_geometry.fft_size);
    return kernel.spectrum_scaling.prepare(fs,std::get<std::span<const float>>(coefficients),kernel.polar_options,kernel.polar_geometry.fft_size)?Status::ok:Status::invalid_argument;
}
void prepare_pair_coordinates(OperationKernelState& kernel,const NumericContract& input) {
    for(auto& lane:kernel.pair_coordinates)for(std::size_t axis=0;axis<2;++axis)
        if(input.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)lane[axis].resize(std::size_t(input.shape.extents[axis].maximum)*sizeof(double));
}
Status prepare_named_fourier(OperationKernelState& kernel,const dsp_kernels::FourierBackendFactory& backend,
    dsp_kernels::FourierBackendTransform transform) {
    if(transform==dsp_kernels::FourierBackendTransform::ComplexForward)kernel.fourier_input.resize(2*kernel.fourier_size);
    auto& operation=kernel.operation.emplace<dsp_kernels::PreparedFourierBackend>();
    return operation.prepare({kernel.fourier_size,transform},backend)?Status::ok:Status::unavailable;
}
Status prepare_fourier(OperationKernelState& kernel,const ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) {
    const dsp_kernels::FourierBackendFactory* backend=nullptr;
    if(!select_fourier_backend(node,profile,backend))return Status::unavailable;
    if(backend)return prepare_named_fourier(kernel,*backend,dsp_kernels::FourierBackendTransform::ComplexForward);
    kernel.operation.emplace<dsp_kernels::FFT>();
    return std::get<dsp_kernels::FFT>(kernel.operation).prepare(kernel.fourier_size,operation_text(node,"radix")=="radix4")?Status::ok:Status::unavailable;
}
Status prepare_precise_fourier(OperationKernelState& kernel,const ResolvedProcessingNode& node) {
    if(!operation_text(node,"backend_id").empty())return Status::unavailable;
    kernel.precise_input.resize(kernel.input.size());kernel.precise_output.resize(kernel.fourier_size+2);
    auto& transform=kernel.operation.emplace<dsp_kernels::detail::PreparedReferenceFourier>();
    return transform.prepare(kernel.fourier_size)?Status::ok:Status::unavailable;
}
Status prepare_real_fourier(OperationKernelState& kernel,const ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) {
    if(std::get<NumericContract>(node.inputs.front().contract.view()).dtype==DType::Float64)return prepare_precise_fourier(kernel,node);
    const dsp_kernels::FourierBackendFactory* backend=nullptr;
    if(!select_fourier_backend(node,profile,backend))return Status::unavailable;
    if(backend)return prepare_named_fourier(kernel,*backend,dsp_kernels::FourierBackendTransform::RealForward);
    kernel.operation.emplace<dsp_kernels::PreparedRfft>();
    return std::get<dsp_kernels::PreparedRfft>(kernel.operation).prepare(kernel.fourier_size,operation_text(node,"radix")=="radix4")?Status::ok:Status::unavailable;
}
void prepare_plane_storage(OperationKernelState& kernel,const NumericContract& input,std::size_t output_samples) {
    kernel.imaginary.resize(kernel.input.size());
    kernel.output.resize(output_samples);
    prepare_pair_coordinates(kernel,input);
}
Status prepare_fourier_planes(OperationKernelState& kernel,const NumericContract& input,
    const ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) {
    const auto samples=kernel.input.size();
    double size=0;
    if(!numeric_parameter(node,"fft_size",size) || !std::isfinite(size) || size<1 || size>UINT32_MAX || std::floor(size)!=size)
        return Status::invalid_argument;
    kernel.fourier_size=std::size_t(size);
    if(input.shape.extents[input.shape.rank==Rank::Matrix?1:0]!=ExtentBounds{kernel.fourier_size,kernel.fourier_size})
        return Status::incompatible_contract;
    const bool real=node.kind=="RealFourier";
    prepare_plane_storage(kernel,input,real?samples+2*(samples/kernel.fourier_size):samples*2);
    return real?prepare_real_fourier(kernel,node,profile):prepare_fourier(kernel,node,profile);
}
Status prepare_polar_planes(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& source) {
    prepare_plane_storage(kernel,std::get<NumericContract>(node.numeric_input),kernel.input.size()*2);
    return prepare_polar(kernel,node,source);
}
Status prepare_rfft(OperationKernelState& kernel,const ResolvedProcessingNode& n) {
    double size=0;
    if(!numeric_parameter(n,"fft_size",size) || !std::isfinite(size) || size<1 || size>UINT32_MAX || std::floor(size)!=size)
        return Status::invalid_argument;
    if(std::size_t(size)!=kernel.input.size())return Status::incompatible_contract;
    kernel.operation.emplace<dsp_kernels::PreparedRfft>();
    return std::get<dsp_kernels::PreparedRfft>(kernel.operation).prepare(std::size_t(size),operation_text(n,"radix")=="radix4")?Status::ok:Status::unavailable;
}
bool projection_options(const ResolvedProcessingNode& source,dsp_kernels::SpectrumProjectionOptions& out) noexcept {
    double exponent=0;
    if(!flag(source,"normalize",out.compensate) || !raw_options(source,out) ||
       !numeric_parameter(source,"magnitude_exponent",exponent) || exponent<0 || exponent>std::numeric_limits<float>::max())return false;
    out.magnitude_exponent=float(exponent);
    return dsp_kernels::parse_spectrum_projection(operation_text(source,"representation"),out.representation);
}
bool projection_scaling(const ResolvedProcessingNode& source,std::size_t size,dsp_kernels::WindowScaling& scaling) {
    scaling.samples=size;
    if(operation_text(source,"scaling_source")=="coefficients")return numeric_parameter(source,"coherent_gain",scaling.sum) && numeric_parameter(source,"power_gain",scaling.sum_squares);
    if(operation_text(source,"scaling_source")!="window")return false;
    dsp_kernels::WindowFunction window;bool periodic=false;
    if(!dsp_kernels::parse_window_function(operation_text(source,"window_function"),window) || !flag(source,"periodic",periodic))return false;
    dsp_kernels::PreparedWindows windows{size,periodic};
    const auto result=dsp_kernels::window_scaling(windows.coefficients(window));if(!result)return false;scaling=*result;return true;
}
Status prepare_projection(OperationKernelState& kernel,PlanNode&,const ResolvedProcessingNode& source) {
    double rate=0;dsp_kernels::SpectrumProjectionGeometry geometry;dsp_kernels::SpectrumProjectionOptions options;dsp_kernels::WindowScaling scaling;
    if(!numeric_parameter(source,"sample_rate",rate) || !projection_geometry(source,kernel.input.size()/2,geometry) ||
       !projection_options(source,options))return Status::invalid_argument;
    scaling={geometry.fft_size,1,1};kernel.bin_hz=rate/double(geometry.fft_size);
    if(options.compensate && !projection_scaling(source,geometry.fft_size,scaling))return Status::invalid_argument;
    kernel.operation.emplace<dsp_kernels::PreparedSpectrumProjection>();
    return std::get<dsp_kernels::PreparedSpectrumProjection>(kernel.operation).prepare(geometry,rate,scaling,options)?Status::ok:Status::invalid_argument;
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
Status prepare_signal_operation(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile) {
    if(n.operation==ProcessingOperation::Fourier)
        return prepare_fourier_planes(kernel,std::get<NumericContract>(node.numeric_input),n,profile);
    if(n.operation==ProcessingOperation::ComplexPolar)return prepare_polar_planes(kernel,node,n);
    if(n.kind=="RFFT")return prepare_rfft(kernel,n);
    if(n.kind=="SpectrumProjection")return prepare_projection(kernel,node,n);
    if(n.kind=="FrequencyNormalizer")return prepare_normalizer(kernel,n);
    return Status::unavailable;
}
}
