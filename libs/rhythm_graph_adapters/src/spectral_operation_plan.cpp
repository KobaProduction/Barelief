// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operation_plan.h"
#include "parameter_values.h"
#include "operation_io.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool bool_parameter(const domain::ResolvedProcessingNode& node,std::string_view key,bool fallback,bool& result) noexcept {
    const auto* value=parameter(node,key);if(!value){result=fallback;return true;}
    const auto* flag=std::get_if<bool>(value);if(!flag)return false;
    result=*flag;return true;
}
bool whitening_parameters(const domain::ResolvedProcessingNode& node,OnsetMagnitudeConfig& config) noexcept {
    return scalar_parameter(node,"frame_period_s",512.f/44100.f,config.whitening.frame_period_s) &&
        scalar_parameter(node,"relax_time_s",250,config.whitening.relax_time_s) &&
        scalar_parameter(node,"floor",1e-4f,config.whitening.floor) &&
        scalar_parameter(node,"compression",0,config.compression) && bool_parameter(node,"whiten",false,config.whiten);
}
bool histogram_parameters(const domain::ResolvedProcessingNode& node,HistogramOnsetConfig& config) noexcept {
    std::size_t method=0;
    if(!integer_parameter(node,"method",0,method) || method>2)return false;
    config.method=HistogramOnsetMethod(method);
    return integer_parameter(node,"histogram_bins",10,config.histogram.bins) &&
        scalar_parameter(node,"histogram_minimum",0,config.histogram.minimum) &&
        scalar_parameter(node,"histogram_maximum",3.14159265358979323846f,config.histogram.maximum) &&
        scalar_parameter(node,"magnitude_threshold",.1f,config.magnitude_threshold);
}
bool kernel_plan(const domain::ResolvedProcessingNode& node,SpectralOperationPlan& plan) noexcept {
    if(node.kind=="SpectralRmsFlux"){plan.kind=SpectralOperationKind::Rms;return true;}
    if(node.kind=="PolarNovelty"){
        std::size_t method=0;if(!integer_parameter(node,"method",0,method) || method>2)return false;
        plan.kind=SpectralOperationKind::Polar;plan.polar=PolarNovelty(method);
        return PreparedPolarNovelty::storage_bytes(plan.bins,plan.polar,plan.kernel_bytes);
    }
    if(node.kind=="HistogramOnset"){
        plan.kind=SpectralOperationKind::Histogram;
        return histogram_parameters(node,plan.histogram) && PreparedHistogramOnset::storage_bytes(plan.bins,plan.histogram,plan.kernel_bytes);
    }
    if(!whitening_parameters(node,plan.magnitude))return false;
    plan.kind=node.kind=="SpectralWhitening"?SpectralOperationKind::Whitening:SpectralOperationKind::Magnitude;
    return plan.kind==SpectralOperationKind::Whitening?
        PreparedSpectralWhitening::storage_bytes(plan.bins,plan.magnitude.whitening,plan.kernel_bytes):
        PreparedOnsetMagnitude::storage_bytes(plan.bins,plan.magnitude,plan.kernel_bytes);
}
bool scalar_control(const NumericContract& contract,DType dtype) noexcept {
    return contract.dtype==dtype && contract.shape.rank==Rank::Scalar && contract.channel_order.size()==1;
}
bool supported_input(const domain::ResolvedProcessingNode& node,const SpectralOperationPlan& plan,domain::PortId port) noexcept {
    NumericContract contract;if(!spectral_port(node,true,port,contract))return false;
    if(port==0 || (port==1 && plan.phase_input())){
        NumericContract magnitude;if(!spectral_port(node,true,0,magnitude))return false;
        return contract.dtype==DType::Float32 && contract.representation.encoding==StorageEncoding::Native &&
            contract.channel_order.size()==1 && contract.shape.rank==Rank::Vector &&
            contract.shape.extents[0]==ExtentBounds{plan.bins,plan.bins} &&
            spectral_same_clock(contract.clock,magnitude.clock) && contract.coordinates==magnitude.coordinates;
    }
    if(port==spectral_control_port)return scalar_control(contract,DType::UInt8);
    if(port==12 || port==13)return (plan.kind==SpectralOperationKind::Whitening || plan.kind==SpectralOperationKind::Magnitude) && scalar_control(contract,DType::Float32);
    if(port==10 || port==11)return plan.kind==SpectralOperationKind::Magnitude && scalar_control(contract,port==10?DType::UInt8:DType::Float32);
    return false;
}
bool supported_inputs(const domain::ResolvedProcessingNode& node,const SpectralOperationPlan& plan) noexcept {
    if(node.inputs.size()>7)return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        if(!supported_input(node,plan,node.inputs[i].id))return false;
        for(std::size_t j=0;j<i;++j)if(node.inputs[i].id==node.inputs[j].id)return false;
    }
    return true;
}
}
bool spectral_operation_kind(std::string_view kind) noexcept {
    return kind=="PolarNovelty" || kind=="HistogramOnset" || kind=="SpectralWhitening" || kind=="OnsetMagnitude" || kind=="SpectralRmsFlux";
}
bool spectral_port(const domain::ResolvedProcessingNode& node,bool input,domain::PortId id,NumericContract& contract) noexcept {
    return numeric_port(node,input,id,contract);
}
bool spectral_same_clock(const ClockContract& a,const ClockContract& b) noexcept {
    return equal_frame_clock(a,b);
}
bool plan_spectral_operation(const domain::ResolvedProcessingNode& node,SpectralOperationPlan& plan) noexcept {
    plan={};NumericContract magnitude;
    if(!spectral_operation_kind(node.kind) || !spectral_port(node,true,0,magnitude) ||
       !vector_capacity(node,domain::PortDirection::Input,0,plan.bins) || !plan.bins || !kernel_plan(node,plan))return false;
    NumericContract phase;if(plan.phase_input() && !spectral_port(node,true,1,phase))return false;
    if(!supported_inputs(node,plan))return false;
    if(plan.kind==SpectralOperationKind::Polar || plan.kind==SpectralOperationKind::Whitening || plan.kind==SpectralOperationKind::Magnitude)
        return dataflow_runtime::numeric_operation_mul_bytes(plan.bins,sizeof(float),plan.output_buffer_bytes);
    return true;
}
}
