#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include <charconv>
#include <cstring>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
void encode_band_coordinates(State& state,std::span<const double> centers) {
    auto& coordinates=state.numeric_outputs[0].coordinates[0];
    for(std::size_t i=0;i<centers.size();++i){
        std::uint64_t bits=0;
        const auto value=centers[i];
        std::memcpy(&bits,&value,8);
        for(std::size_t b=0;b<8;++b)coordinates[i*8+b]=std::byte((bits>>(b*8))&255);
    }
}
Status prepare_broadband(OperationKernelState& kernel,PlanNode&,State& state,const ResolvedProcessingNode& node) {
    ProcessingParameterView supplied;
    double bin_hz=0;
    if(!numeric_parameter(node,"bin_hz",bin_hz) || bin_hz<=0 ||
       resolve_processing_parameter(node,"boundaries_hz",supplied)!=Status::ok)return Status::invalid_argument;
    const auto* values=std::get_if<std::span<const float>>(&supplied);
    if(!values)return Status::invalid_argument;
    std::vector<double> boundaries(values->begin(),values->end());
    const auto mode=operation_text(node,"statistic");
    using B=dsp_kernels::BandStatistic;
    const auto statistic=mode=="mean_power"?B::MeanPower:mode=="rms"?B::Rms:
        mode=="maximum"?B::Maximum:mode=="flux"?B::Flux:B::SumEnergy;
    kernel.operation.emplace<dsp_kernels::BroadbandFeatures>();
    auto& feature=std::get<dsp_kernels::BroadbandFeatures>(kernel.operation);
    if(!feature.prepare(kernel.input.size(),bin_hz,boundaries,statistic))return Status::invalid_argument;
    encode_band_coordinates(state,feature.centers_hz());
    return Status::ok;
}
Status prepare_quantizer(OperationKernelState& kernel,const ResolvedProcessingNode& n) {

        double bits=0,minimum=0,maximum=0;if(!numeric_parameter(n,"bits",bits) || !numeric_parameter(n,"min_value",minimum) || !numeric_parameter(n,"max_value",maximum))return Status::invalid_argument;
        kernel.operation.emplace<dsp_kernels::SpectralQuantizer>();
        if(!std::get<dsp_kernels::SpectralQuantizer>(kernel.operation).prepare(minimum,maximum,unsigned(bits)))return Status::invalid_argument;
        if(bits==8)kernel.quantized8.resize(kernel.output.size());else kernel.quantized16.resize(kernel.output.size());
        return Status::ok;
}
}
Status prepare_signal_features(OperationKernelState& kernel,PlanNode& node,State& state,const ResolvedProcessingNode& n) {
    if(n.kind=="BroadbandFeatures")return prepare_broadband(kernel,node,state,n);
    if(n.kind=="SpectralQuantize")return prepare_quantizer(kernel,n);
    return Status::unavailable;
}
}
