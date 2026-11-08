// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operation_plan.h"
#include "parameter_values.h"
#include "operation_io.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool whitening_parameters(const dataflow_runtime::NumericOperationNodeView& node,SpectralWhiteningConfig& config) noexcept {
    return scalar_parameter(node,"frame_period_s",512.f/44100.f,config.frame_period_s) &&
        scalar_parameter(node,"relax_time_s",250,config.relax_time_s) &&
        scalar_parameter(node,"floor",1e-4f,config.floor);
}
bool histogram_parameters(const dataflow_runtime::NumericOperationNodeView& node,HistogramOnsetConfig& config) noexcept {
    std::size_t method=0;
    if(!integer_parameter(node,"method",0,method) || method>2)return false;
    config.method=HistogramOnsetMethod(method);
    return integer_parameter(node,"histogram_bins",10,config.histogram.bins) &&
        scalar_parameter(node,"histogram_minimum",0,config.histogram.minimum) &&
        scalar_parameter(node,"histogram_maximum",3.14159265358979323846f,config.histogram.maximum) &&
        scalar_parameter(node,"magnitude_threshold",.1f,config.magnitude_threshold);
}
bool polar_plan(const dataflow_runtime::NumericOperationNodeView& node,SpectralOperationPlan& plan) noexcept {
    std::size_t method=0;if(!integer_parameter(node,"method",0,method) || method>2)return false;
    plan.kind=SpectralOperationKind::Polar;plan.polar=PolarNovelty(method);
    return PreparedPolarNovelty::storage_bytes(plan.bins,plan.polar,plan.kernel_bytes);
}
bool whitening_plan(const dataflow_runtime::NumericOperationNodeView& node,SpectralOperationPlan& plan) noexcept {
    plan.kind=SpectralOperationKind::Whitening;
    return whitening_parameters(node,plan.whitening) &&
        PreparedSpectralWhitening::storage_bytes(plan.bins,plan.whitening,plan.kernel_bytes);
}
bool kernel_plan(const dataflow_runtime::NumericOperationNodeView& node,SpectralOperationPlan& plan) noexcept {
    if(node.kind=="SpectralRmsFlux"){plan.kind=SpectralOperationKind::Rms;return true;}
    if(node.kind=="PolarNovelty")return polar_plan(node,plan);
    if(node.kind=="HistogramOnset"){
        plan.kind=SpectralOperationKind::Histogram;
        return histogram_parameters(node,plan.histogram) && PreparedHistogramOnset::storage_bytes(plan.bins,plan.histogram,plan.kernel_bytes);
    }
    return whitening_plan(node,plan);
}
bool scalar_control(const NumericBufferContract& contract,DType dtype) noexcept {
    return contract.dtype==dtype && contract.shape.rank==Rank::Scalar && contract.plane_count==1;
}
bool spectral_frame_contract(const NumericBufferContract& contract,const SpectralOperationPlan& plan) noexcept {
    return contract.dtype==DType::Float32 && contract.representation.encoding==StorageEncoding::Native && contract.plane_count==1 &&
        contract.shape.rank==Rank::Vector && contract.shape.extents[0]==ExtentBounds{plan.bins,plan.bins};
}
bool spectral_control_contract(const NumericBufferContract& contract,const SpectralOperationPlan& plan,domain::PortId port) noexcept {
    if(port==spectral_control_port)return scalar_control(contract,DType::UInt8);
    if(port==12 || port==13)return plan.kind==SpectralOperationKind::Whitening && scalar_control(contract,DType::Float32);
    return false;
}
bool supported_input(const dataflow_runtime::NumericOperationNodeView& node,const SpectralOperationPlan& plan,domain::PortId port) noexcept {
    NumericBufferContract contract;if(!spectral_port(node,true,port,contract))return false;
    if(port==0 || (port==1 && plan.phase_input())){
        NumericBufferContract magnitude;if(!spectral_port(node,true,0,magnitude))return false;
        return spectral_frame_contract(contract,plan);
    }
    return spectral_control_contract(contract,plan,port);
}
bool supported_inputs(const dataflow_runtime::NumericOperationNodeView& node,const SpectralOperationPlan& plan) noexcept {
    if(node.inputs.size()>7)return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        if(!supported_input(node,plan,node.inputs[i].id))return false;
        if(!operation_port_unique(node.inputs,i))return false;
    }
    return true;
}
}
bool spectral_operation_kind(std::string_view kind) noexcept {
    return kind=="PolarNovelty" || kind=="HistogramOnset" || kind=="SpectralWhitening" || kind=="SpectralRmsFlux";
}
bool spectral_port(const dataflow_runtime::NumericOperationNodeView& node,bool input,domain::PortId id,NumericBufferContract& contract) noexcept {
    return numeric_port(node,input,id,contract);
}

bool plan_spectral_operation(const dataflow_runtime::NumericOperationNodeView& node,SpectralOperationPlan& plan) noexcept {
    plan={};NumericBufferContract magnitude;
    if(!spectral_operation_kind(node.kind) || !spectral_port(node,true,0,magnitude) ||
       !vector_capacity(node,domain::PortDirection::Input,0,plan.bins) || !plan.bins || !kernel_plan(node,plan))return false;
    NumericBufferContract phase;if(plan.phase_input() && !spectral_port(node,true,1,phase))return false;
    if(!supported_inputs(node,plan))return false;
    if(plan.kind==SpectralOperationKind::Polar || plan.kind==SpectralOperationKind::Whitening)
        return dataflow_runtime::numeric_operation_mul_bytes(plan.bins,sizeof(float),plan.output_buffer_bytes);
    return true;
}
}
