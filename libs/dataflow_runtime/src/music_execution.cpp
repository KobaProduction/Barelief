#include "music_execution.h"
#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <domain/music/contracts.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <dsp_kernels/musical_features.h>
#include <dsp_kernels/frequency_profile.h>
#include <cmath>
#include <limits>

namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
std::string_view operation_text(const ResolvedProcessingNode& n,std::string_view key) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(n,key,value)!=Status::ok)return {};
    const auto* result=std::get_if<std::string_view>(&value);return result?*result:std::string_view{};
}


PreparedOperationKernel::PreparedOperationKernel():value_(std::make_unique<OperationKernelState>()){}
PreparedOperationKernel::~PreparedOperationKernel()=default;
PreparedOperationKernel::PreparedOperationKernel(const PreparedOperationKernel& other):value_(std::make_unique<OperationKernelState>(*other.value_)){}
PreparedOperationKernel& PreparedOperationKernel::operator=(const PreparedOperationKernel& other){if(this!=&other)value_=std::make_unique<OperationKernelState>(*other.value_);return *this;}
PreparedOperationKernel::PreparedOperationKernel(PreparedOperationKernel&&) noexcept=default;
PreparedOperationKernel& PreparedOperationKernel::operator=(PreparedOperationKernel&&) noexcept=default;
namespace {
void reset_pairs(OperationKernelState& state) noexcept {
    state.pending={};state.product_pending=state.product_triggered=false;
    state.onset_position=0;state.onset_filled=0;
    std::fill(state.factor_actual.begin(),state.factor_actual.end(),std::array<std::size_t,2>{});
    std::copy(state.factor_initial_ready.begin(),state.factor_initial_ready.end(),state.factor_ready.begin());
}
}
void PreparedOperationKernel::reset() noexcept {reset_pairs(*value_);if(auto* broad=std::get_if<dsp_kernels::BroadbandFeatures>(&value_->operation))broad->reset();if(auto* descriptor=std::get_if<dsp_kernels::PreparedSpectralDescriptor>(&value_->operation))descriptor->reset();if(value_->rhythm)value_->rhythm->reset();if(value_->tempo)value_->tempo->reset();}
void PreparedOperationKernel::invalidate_product_lane(std::size_t lane) noexcept {
    if(lane>=value_->factor_ready.size())return;
    value_->factor_ready[lane]=false;value_->product_pending=value_->product_triggered=false;
}
namespace {
bool copy_onset_state(OperationKernelState& target,const OperationKernelState& source) noexcept {
    if(target.onset_lag!=source.onset_lag || target.onset_history.size()!=source.onset_history.size())return false;
    std::copy(source.onset_history.begin(),source.onset_history.end(),target.onset_history.begin());
    target.onset_position=source.onset_position;target.onset_filled=source.onset_filled;return true;
}
}

bool PreparedOperationKernel::copy_state_from(const PreparedOperationKernel& other) noexcept {
    if(value_->onset_lag)return copy_onset_state(*value_,*other.value_);
    if(auto* current=std::get_if<dsp_kernels::PreparedSpectralDescriptor>(&value_->operation)){
        const auto* before=std::get_if<dsp_kernels::PreparedSpectralDescriptor>(&other.value_->operation);
        return before && current->copy_state_from(*before);
    }
    if(value_->rhythm)return other.value_->rhythm && value_->rhythm->copy_state_from(*other.value_->rhythm);
    auto* current=std::get_if<dsp_kernels::BroadbandFeatures>(&value_->operation);
    const auto* before=std::get_if<dsp_kernels::BroadbandFeatures>(&other.value_->operation);
    return !current || (before && current->copy_state_from(*before));
}

void PreparedOperationKernel::copy_rhythm_clipping_stats(NodeStats& stats) const noexcept {
    if(!value_->rhythm)return;
    const auto clipping=value_->rhythm->clipping_statistics();
    stats.clipped_frames=clipping.clipped_frames;
    stats.clipped_values=clipping.clipped_values;
    stats.maximum_before_clamp=clipping.maximum_before_clamp;
}
namespace {
bool assign_resampler_config(const ResolvedProcessingNode& node,const NumericContract& input,double input_rate,double output_rate,double quality,dsp_kernels::ResamplerConfig& config) noexcept {
    config.input_rate=std::uint32_t(input_rate);
    config.output_rate=std::uint32_t(output_rate);
    config.channels=input.channel_order.size();
    config.quality=unsigned(quality);
    if(!node.enabled)config.mode=dsp_kernels::ResamplerMode::Bypass;
    else if(!signal_contracts::parse_resampler_mode(operation_text(node,"mode"),config.mode))return false;
    if(config.mode==dsp_kernels::ResamplerMode::Bypass)config.output_rate=config.input_rate;
    return true;
}
}
bool resampler_config(const ResolvedProcessingNode& node,dsp_kernels::ResamplerConfig& config) noexcept {
    if(node.kind!="PcmResampler" || node.inputs.empty())return false;
    const auto input=std::get<NumericContract>(node.inputs.front().contract.view());
    double input_rate=0,output_rate=0,quality=0;
    if(!numeric_parameter(node,"input_rate",input_rate) ||
       !numeric_parameter(node,"output_rate",output_rate) ||
       !numeric_parameter(node,"quality",quality) ||
       input_rate<1 || input_rate>UINT32_MAX || std::floor(input_rate)!=input_rate ||
       output_rate<1 || output_rate>UINT32_MAX || std::floor(output_rate)!=output_rate)return false;
    return assign_resampler_config(node,input,input_rate,output_rate,quality,config);
}
namespace {
bool concat_coordinate_resources(const NumericContract& input,ProcessingResources& resources) noexcept {
    if(input.shape.rank!=Rank::Matrix)return true;
    for(std::size_t axis=0;axis<2;++axis)if(input.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        std::size_t bytes=0;
        if(!numeric_mul(std::size_t(input.shape.extents[axis].maximum),sizeof(double),bytes) || !numeric_add(resources.payload_bytes,bytes))return false;
    }
    return true;
}
bool concat_resources(const ResolvedProcessingNode& node,ProcessingResources& resources) noexcept {
    const auto right=std::get<NumericContract>(node.inputs[1].contract.view());
    std::size_t count=std::size_t(right.shape.extents[0].maximum),bytes=0;
    if(right.shape.rank==Rank::Matrix && !numeric_mul(count,std::size_t(right.shape.extents[1].maximum),count))return false;
    if(!numeric_mul(count,sizeof(float),bytes) || !numeric_add(resources.payload_bytes,bytes))return false;
    for(const auto& port:node.inputs)
        if(!concat_coordinate_resources(std::get<NumericContract>(port.contract.view()),resources))return false;
    return true;
}
std::optional<bool> numeric_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,const NumericRuntimeProfile& profile,std::size_t input) {
    const auto scalar_bytes=std::get<NumericContract>(n.outputs.front().contract.view()).dtype==DType::Float64?sizeof(double):sizeof(float);
    if(n.operation==ProcessingOperation::ArrayProduct)return numeric_add(resources.payload_bytes,profile.admission.frame.max_string_bytes+n.inputs.size()*(input*scalar_bytes+2*sizeof(std::uint8_t)+sizeof(std::array<std::size_t,2>)));
    if(n.operation!=ProcessingOperation::ArrayConcat)return {};
    return concat_resources(n,resources);
}
std::optional<bool> feature_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,std::size_t input,std::size_t output,const NumericContract& result) {
    if(n.kind=="RhythmFeatures")return numeric_add(resources.scratch_bytes,96*sizeof(float));
    if(n.kind=="BroadbandFeatures")return numeric_add(resources.scratch_bytes,input*sizeof(float)+output*48+8);
    if(n.kind=="SpectralQuantize")return numeric_add(resources.payload_bytes,output*(result.dtype==DType::UInt8?1:2));
    if(n.kind=="FrequencyNormalizer")return numeric_add(resources.scratch_bytes,input*sizeof(float));
    return {};
}
bool projection_operation_resources(const ResolvedProcessingNode& node,ProcessingResources& resources,std::size_t output) {
    if(!numeric_add(resources.scratch_bytes,output*sizeof(float)))return false;
    ProcessingParameterView normalized;double size=0;std::size_t bytes=0;
    if(resolve_processing_parameter(node,"normalize",normalized)!=Status::ok || !std::holds_alternative<bool>(normalized) || !numeric_parameter(node,"fft_size",size))return false;
    if(!std::get<bool>(normalized) || operation_text(node,"scaling_source")!="window")return true;
    return dsp_kernels::PreparedWindows::storage_bytes(std::size_t(size),bytes) && numeric_add(resources.scratch_bytes,bytes);
}
bool paired_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,std::size_t input) {
    std::size_t coordinates=0;const auto contract=std::get<NumericContract>(n.inputs.front().contract.view());
    for(std::size_t axis=0;axis<2;++axis)if(contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
        {std::size_t bytes=0;if(!numeric_mul(std::size_t(contract.shape.extents[axis].maximum),2*sizeof(double),bytes) || !numeric_add(coordinates,bytes))return false;}
    return numeric_add(resources.payload_bytes,(4*input+2)*sizeof(float)) && numeric_add(resources.payload_bytes,coordinates) && numeric_add(resources.scratch_bytes,(4*input+4)*sizeof(float));
}

bool account_fourier_backend(ProcessingResources& resources,const dsp_kernels::FourierBackendResources& required,
    std::size_t count,bool real) {
    std::size_t staging=0;
    if(!real && !numeric_mul(count,2*sizeof(float),staging))return false;
    return numeric_add(resources.scratch_bytes,staging) && numeric_add(resources.scratch_bytes,required.cold_peak_bytes) &&
        numeric_add(resources.payload_bytes,required.shared_bytes) && numeric_add(resources.stack_bytes,required.hot_stack_bytes);
}
bool fourier_backend_resources(const ResolvedProcessingNode& node,ProcessingResources& resources,const NumericRuntimeProfile& profile) {
    if(node.kind!="Fourier" && node.kind!="RealFourier")return true;
    const dsp_kernels::FourierBackendFactory* backend=nullptr;
    if(!select_fourier_backend(node,profile,backend))return false;
    if(!backend)return true;
    const auto input=std::get<NumericContract>(node.inputs.front().contract.view());
    const auto count=std::size_t(input.shape.extents[input.shape.rank==Rank::Matrix?1:0].maximum);
    const bool real=node.kind=="RealFourier";
    dsp_kernels::FourierBackendResources required;
    const auto transform=real?dsp_kernels::FourierBackendTransform::RealForward:dsp_kernels::FourierBackendTransform::ComplexForward;
    if(!dsp_kernels::PreparedFourierBackend::query({count,transform},*backend,required))return false;
    return account_fourier_backend(resources,required,count,real);
}
bool precise_fourier_resources(const ResolvedProcessingNode& node,ProcessingResources& resources,std::size_t input) {
    const auto contract=std::get<NumericContract>(node.inputs.front().contract.view());
    if(contract.dtype!=DType::Float64)return true;
    const auto count=std::size_t(contract.shape.extents[contract.shape.rank==Rank::Matrix?1:0].maximum);
    std::size_t bytes=0;
    return node.kind=="RealFourier" && operation_text(node,"backend_id").empty() &&
        dsp_kernels::detail::PreparedReferenceFourier::storage_bytes(count,bytes) &&
        numeric_add(resources.scratch_bytes,bytes) && numeric_add(resources.payload_bytes,(input+count+2)*sizeof(double));
}
std::optional<bool> signal_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,std::size_t input,std::size_t output,const NumericContract& result,const NumericRuntimeProfile& profile) {
    if(spectral_descriptor_kind(n.kind)){
        std::size_t bytes=0;return dsp_kernels::PreparedSpectralDescriptor::storage_bytes(input/2,bytes) && numeric_add(resources.scratch_bytes,bytes);
    }
    if(paired_processing(n.operation))return paired_operation_resources(n,resources,input) && fourier_backend_resources(n,resources,profile) && precise_fourier_resources(n,resources,input);
    if(n.kind=="RFFT")return numeric_add(resources.scratch_bytes,(3*input+4)*sizeof(float));
    if(n.kind=="SpectrumProjection")return projection_operation_resources(n,resources,output);
    return feature_operation_resources(n,resources,input,output,result);
}
bool music_operation_resources(const ResolvedProcessingNode& node,ProcessingResources& resources,std::size_t input,std::size_t output,const NumericContract&){
    if(node.kind=="MusicalSpectrogram" || node.kind=="BroadbandRow" || node.kind=="NoteSpectrum")return numeric_add(resources.scratch_bytes,input*128+output*128+4096);
    if(node.kind!="NoteSalience")return false;
    double first=0,harmonics=0;
    if(!numeric_parameter(node,"first_bin",first) || (first!=0 && first!=1) || !numeric_parameter(node,"harmonics",harmonics))return false;
    std::size_t bins=input;if(!numeric_add(bins,std::size_t(first)))return false;
    if(!numeric_add(resources.payload_bytes,bins*sizeof(float)))return false;
    return numeric_add(resources.scratch_bytes,input*sizeof(float)+output*(48+std::size_t(harmonics)*40));
}
}
namespace {
bool transform_geometry_supported(const ResolvedProcessingNode& node,const NumericContract& input) noexcept {
    if(node.kind!="Fourier" && node.kind!="RealFourier" && node.kind!="RFFT")return true;
    const auto minimum=node.kind=="Fourier"?4u:8u;
    const auto count=std::size_t(input.shape.extents[input.shape.rank==Rank::Matrix?1:0].maximum);
    return count>=minimum && count<=dsp_kernels::max_fft_samples && !(count&(count-1));
}
bool operation_support(const ResolvedProcessingNode& node,ProcessingResources& resources,const NumericRuntimeProfile& profile,const NumericContract& input,const NumericContract& output) noexcept {
    const auto& buffer=node.operation==ProcessingOperation::ArrayProduct?output:input;
    const auto in=std::size_t(buffer.shape.extents[0].maximum)*(buffer.shape.rank==Rank::Matrix?std::size_t(buffer.shape.extents[1].maximum):1);
    const auto out=std::size_t(output.shape.extents[0].maximum)*(output.shape.rank==Rank::Matrix?std::size_t(output.shape.extents[1].maximum):1);
    if(!transform_geometry_supported(node,input))return false;
    std::size_t count=in,bytes=0;
    if(!numeric_add(count,out) || !numeric_mul(count,sizeof(float),bytes) ||
       !numeric_add(resources.plan_bytes,sizeof(OperationKernelState)) || !numeric_add(resources.payload_bytes,bytes))return false;
    if(const auto numeric=numeric_operation_resources(node,resources,profile,in))return *numeric;
    if(const auto signal=signal_operation_resources(node,resources,in,out,output,profile))return *signal;
    return music_operation_resources(node,resources,in,out,input);
}
void prepare_operation_buffers(OperationKernelState& kernel,const PlanNode& node) {
    const auto output=std::get<NumericContract>(node.numeric_outputs[0]);
    const auto input=node.operation==ProcessingOperation::ArrayProduct?output:std::get<NumericContract>(node.numeric_input);
    kernel.input.resize(std::size_t(input.shape.extents[0].maximum)*(input.shape.rank==Rank::Matrix?std::size_t(input.shape.extents[1].maximum):1));
    kernel.output.resize(std::size_t(output.shape.extents[0].maximum)*(output.shape.rank==Rank::Matrix?std::size_t(output.shape.extents[1].maximum):1));
}
}
bool music_support(const ResolvedProcessingNode& node,ProcessingResources& resources,const NumericRuntimeProfile& profile) noexcept {
    if(node.kind=="Mel" || node.kind=="PowerToDb")return spectral_frontend_resources(node,resources);
    if(node.kind=="OnsetStrength")return onset_strength_resources(node,resources);
    if(node.kind=="TempoEvidence")return numeric_add(resources.plan_bytes,sizeof(OperationKernelState)) && PreparedTempoEvidenceOperation::resources(node,resources);
    if(node.kind=="RhythmAnalyzer")return numeric_add(resources.plan_bytes,sizeof(OperationKernelState)) && numeric_add(resources.scratch_bytes,dsp_kernels::rhythm::Analyzer::workspace_bytes()+96*sizeof(float));
    const auto input=std::get<NumericContract>(node.inputs.front().contract.view());
    const auto output=std::get<NumericContract>(node.outputs.front().contract.view());
    return operation_support(node,resources,profile,input,output);
}
namespace {
std::optional<Status> prepare_temporal_operation(OperationKernelState& kernel,State& state,const ResolvedProcessingNode& node) {
    if(node.kind=="Mel" || node.kind=="PowerToDb")return prepare_spectral_frontend(kernel,state,node);
    if(node.kind=="OnsetStrength")return prepare_onset_strength(kernel,node);
    if(node.kind=="RhythmAnalyzer")return prepare_rhythm(kernel,node);
    if(node.kind!="TempoEvidence")return {};
    kernel.tempo.emplace();return kernel.tempo->prepare(node)?Status::ok:Status::invalid_argument;
}
}
Status prepare_music(PlanNode& node,State& state,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile){
    state.kernel.emplace<PreparedOperationKernel>();auto& kernel=*std::get<PreparedOperationKernel>(state.kernel).value_;
    if(const auto temporal=prepare_temporal_operation(kernel,state,n))return *temporal;
    prepare_operation_buffers(kernel,node);
    const auto numeric=prepare_numeric_operation(kernel,node,n,profile);
    if(numeric!=Status::unavailable)return numeric;
    const auto signal=prepare_signal_operation(kernel,node,n,profile);
    if(signal!=Status::unavailable)return signal;
    const auto feature=prepare_signal_features(kernel,node,state,n);
    if(feature!=Status::unavailable)return feature;
    return prepare_music_operation(kernel,node,state,n);
}
namespace {
Status load_operation_input(OperationKernelState& kernel,const NumericView& source,std::size_t rows,std::size_t columns) noexcept {
    for(std::size_t row=0;row<rows;++row)for(std::size_t column=0;column<columns;++column){
        const auto status=load_element(source,{0,row,column},kernel.input[row*columns+column]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
double axis_value(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<sizeof(double);++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*sizeof(double)+byte]))<<(8*byte);
    double value=0;std::memcpy(&value,&bits,sizeof(value));return value;
}
bool projection_coordinates(const OperationKernelState& kernel,const NumericView& source) noexcept {
    const auto* projection=std::get_if<dsp_kernels::PreparedSpectrumProjection>(&kernel.operation);
    if(!projection || source.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE || source.contract->coordinates[0].unit!=NumericUnit::Hertz)return true;
    const auto bytes=source.coordinates.axes[0];if(bytes.size()!=source.actual[0]*sizeof(double))return false;
    const auto first=projection->geometry().first_bin;
    for(std::size_t bin=0;bin<source.actual[0];++bin){
        const auto actual=axis_value(bytes,bin),expected=double(first+bin)*kernel.bin_hz;
        if(!std::isfinite(actual) || std::abs(actual-expected)>1e-9*std::max(1.,std::abs(expected)))return false;
    }
    return true;
}
Status read_operation_input(OperationKernelState& kernel,const NumericView& source,const FrameLimits& limits) noexcept {
    const auto rows=source.actual[0],columns=source.contract->shape.rank==Rank::Matrix?source.actual[1]:1;
    if(source.ring.capacity || rows*columns!=kernel.input.size() || !projection_coordinates(kernel,source))return Status::incompatible_contract;
    const auto valid=validate_values(source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok)return valid;
    return load_operation_input(kernel,source,rows,columns);
}
Status write_operation_output(const OperationKernelState& kernel,MutableNumericView& destination) noexcept {
    const auto columns=destination.contract->shape.rank==Rank::Matrix?destination.actual[1]:1;
    for(std::size_t row=0;row<destination.actual[0];++row)for(std::size_t column=0;column<columns;++column){
        const auto status=store_element(destination,{0,row,column},kernel.output[row*columns+column]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status quantize_operation(OperationKernelState& kernel,MutableNumericView& destination,dsp_kernels::SpectralQuantizer& quantizer) noexcept {
    const bool bytes=destination.contract->dtype==DType::UInt8;
    const bool success=bytes?quantizer.process(kernel.input,kernel.quantized8):quantizer.process(kernel.input,kernel.quantized16);
    if(!success)return Status::overflow;
    for(std::size_t i=0;i<kernel.input.size();++i){
        const auto bits=bytes?std::uint64_t(kernel.quantized8[i]):std::uint64_t(kernel.quantized16[i]);
        const auto status=store_bits(destination,{0,i,0},destination.contract->dtype,bits);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status compute_operation(OperationKernelState& kernel,MutableNumericView& destination,const DataFrameView& current,bool enabled,bool boundary,const FrameLimits& limits) noexcept {
    if(!enabled){
        if(auto* temporal=std::get_if<dsp_kernels::BroadbandFeatures>(&kernel.operation))temporal->reset();
        if(auto* descriptor=std::get_if<dsp_kernels::PreparedSpectralDescriptor>(&kernel.operation))descriptor->reset();
        return fill_silence(destination,limits);
    }
    if(auto* quantizer=std::get_if<dsp_kernels::SpectralQuantizer>(&kernel.operation))return quantize_operation(kernel,destination,*quantizer);
    const auto processed=process_prepared_operation(kernel,current.stamp.discontinuity || boundary);
    return processed==Status::ok?write_operation_output(kernel,destination):processed;
}
Status copy_quantizer_coordinates(const NumericView& source,State& state) noexcept {
    if(source.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    auto& coordinates=state.numeric_outputs[0].coordinates[0];
    if(source.coordinates.axes[0].size()!=coordinates.size())return Status::incompatible_contract;
    std::copy(source.coordinates.axes[0].begin(),source.coordinates.axes[0].end(),coordinates.begin());return Status::ok;
}
Status execute_common_music(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    auto destination=mutable_output(node,state,0);
    const auto status=compute_operation(kernel,destination,current,node.node.enabled,state.pending_discontinuity,limits);
    if(status==Status::ok)state.numeric_frames[0]={current.stamp,destination.view()};
    return status;
}
Status execute_loaded_music(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(current.payload);
    if(node.node.kind==NodeKind::RhythmFeatures)return execute_rhythm_features(kernel,node,state,current);
    if(node.node.kind==NodeKind::SpectralQuantize || node.node.kind==NodeKind::SpectrumProjection){const auto copied=copy_quantizer_coordinates(source,state);if(copied!=Status::ok)return copied;}
    if(std::holds_alternative<dsp_kernels::MusicalSpectrogram>(kernel.operation))return execute_musical_spectrogram(kernel,node,state,current);
    return execute_common_music(kernel,node,state,current,limits);
}
}
Status execute_music(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    auto& kernel=*std::get<PreparedOperationKernel>(state.kernel).value_;
    if(kernel.rhythm)return execute_rhythm(kernel,node,state,current,limits);
    if(kernel.onset_strength)return execute_onset_strength(kernel,node,state,current,limits);
    if(kernel.mel || kernel.db)return execute_spectral_frontend(kernel,node,state,current,limits);
    const auto& source=std::get<NumericView>(current.payload);
    const auto read=read_operation_input(kernel,source,limits);if(read!=Status::ok)return read;
    return execute_loaded_music(kernel,node,state,current,limits);
}
}
