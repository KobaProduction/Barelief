#include "music_execution.h"
#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
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
void PreparedOperationKernel::reset() noexcept {value_->pending={};value_->product_pending=false;std::copy(value_->factor_initial_ready.begin(),value_->factor_initial_ready.end(),value_->factor_ready.begin());if(auto* broad=std::get_if<dsp_kernels::BroadbandFeatures>(&value_->operation))broad->reset();}
void PreparedOperationKernel::invalidate_product_lane(std::size_t lane) noexcept {
    if(lane>=value_->factor_ready.size())return;
    value_->factor_ready[lane]=false;value_->product_pending=false;
}
bool PreparedOperationKernel::copy_state_from(const PreparedOperationKernel& other) noexcept {
    auto* current=std::get_if<dsp_kernels::BroadbandFeatures>(&value_->operation);
    const auto* before=std::get_if<dsp_kernels::BroadbandFeatures>(&other.value_->operation);
    return !current || (before && current->copy_state_from(*before));
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
std::optional<bool> numeric_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,const NumericRuntimeProfile& profile,std::size_t input) {
    if(n.operation==ProcessingOperation::ArrayProduct)return numeric_add(resources.payload_bytes,profile.admission.frame.max_string_bytes+n.inputs.size()*(input*sizeof(float)+2*sizeof(std::uint8_t)));
    if(n.operation!=ProcessingOperation::ArrayConcat)return {};
    const auto right=n.inputs[1].contract.view();
    return numeric_add(resources.payload_bytes,std::size_t(std::get<NumericContract>(right).shape.extents[0].maximum)*sizeof(float));
}
std::optional<bool> feature_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,std::size_t input,std::size_t output,const NumericContract& result) {
    if(n.kind=="BroadbandFeatures")return numeric_add(resources.scratch_bytes,input*sizeof(float)+output*48+8);
    if(n.kind=="SpectralQuantize")return numeric_add(resources.payload_bytes,output*(result.dtype==DType::UInt8?1:2));
    if(n.kind=="FrequencyNormalizer")return numeric_add(resources.scratch_bytes,input*sizeof(float));
    return {};
}
std::optional<bool> signal_operation_resources(const ResolvedProcessingNode& n,ProcessingResources& resources,std::size_t input,std::size_t output,const NumericContract& result) {
    if(paired_processing(n.operation))return numeric_add(resources.payload_bytes,(4*input+2)*sizeof(float)) && numeric_add(resources.scratch_bytes,(4*input+4)*sizeof(float));
    if(n.kind=="RFFT")return numeric_add(resources.scratch_bytes,(3*input+4)*sizeof(float));
    if(n.kind=="SpectrumProjection"){
        std::size_t windows=0;if(!dsp_kernels::PreparedWindows::storage_bytes(input-2,windows))return false;
        return numeric_add(resources.scratch_bytes,output*sizeof(float)+windows);
    }
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
bool music_support(const ResolvedProcessingNode& node,ProcessingResources& resources,const NumericRuntimeProfile& profile) noexcept {
    const auto input=std::get<NumericContract>(node.inputs.front().contract.view());
    const auto output=std::get<NumericContract>(node.outputs.front().contract.view());
    const auto in=std::size_t(input.shape.extents[0].maximum)*(input.shape.rank==Rank::Matrix?std::size_t(input.shape.extents[1].maximum):1);
    const auto out=std::size_t(output.shape.extents[0].maximum)*(output.shape.rank==Rank::Matrix?std::size_t(output.shape.extents[1].maximum):1);
    const bool fourier=node.kind=="Fourier" || node.kind=="RealFourier" || node.kind=="RFFT";
    if(fourier && (in<256 || in>dsp_kernels::max_fft_samples || (in&(in-1))))return false;
    if(!numeric_add(resources.plan_bytes,sizeof(OperationKernelState)) || !numeric_add(resources.payload_bytes,(in+out)*sizeof(float)))return false;
    if(const auto numeric=numeric_operation_resources(node,resources,profile,in))return *numeric;
    if(const auto signal=signal_operation_resources(node,resources,in,out,output))return *signal;
    return music_operation_resources(node,resources,in,out,input);
}
Status prepare_music(PlanNode& node,State& state,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile){
    state.kernel.emplace<PreparedOperationKernel>();auto& kernel=*std::get<PreparedOperationKernel>(state.kernel).value_;
    const auto input=std::get<NumericContract>(node.numeric_input);
    const auto output=std::get<NumericContract>(node.numeric_outputs[0]);
    kernel.input.resize(std::size_t(input.shape.extents[0].maximum)*(input.shape.rank==Rank::Matrix?std::size_t(input.shape.extents[1].maximum):1));
    kernel.output.resize(std::size_t(output.shape.extents[0].maximum)*(output.shape.rank==Rank::Matrix?std::size_t(output.shape.extents[1].maximum):1));
    const auto numeric=prepare_numeric_operation(kernel,node,n,profile);
    if(numeric!=Status::unavailable)return numeric;
    const auto signal=prepare_signal_operation(kernel,node,n);
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
Status read_operation_input(OperationKernelState& kernel,const NumericView& source,const FrameLimits& limits) noexcept {
    const auto rows=source.actual[0],columns=source.contract->shape.rank==Rank::Matrix?source.actual[1]:1;
    if(source.ring.capacity || rows*columns!=kernel.input.size())return Status::incompatible_contract;
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
}
Status execute_music(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    auto& kernel=*std::get<PreparedOperationKernel>(state.kernel).value_;
    const auto& source=std::get<NumericView>(current.payload);
    const auto read=read_operation_input(kernel,source,limits);if(read!=Status::ok)return read;
    if(node.node.kind==NodeKind::SpectralQuantize){const auto copied=copy_quantizer_coordinates(source,state);if(copied!=Status::ok)return copied;}
    if(std::holds_alternative<dsp_kernels::MusicalSpectrogram>(kernel.operation))return execute_musical_spectrogram(kernel,node,state,current);
    return execute_common_music(kernel,node,state,current,limits);
}
}
