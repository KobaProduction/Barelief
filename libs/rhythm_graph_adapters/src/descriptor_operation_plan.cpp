// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool onset_plan(const dataflow_runtime::NumericOperationNodeView& node,DescriptorPlan& plan) noexcept {
    std::size_t method=1;OnsetDescriptorRecipe recipe;
    if(!integer_parameter(node,"method",1,method) || method>9)return false;
    plan.kind=DescriptorKind::Onset;plan.recipe=OnsetRecipe(method);
    plan.phase=plan.recipe==OnsetRecipe::Complex || plan.recipe==OnsetRecipe::Phase || plan.recipe==OnsetRecipe::WeightedPhase;
    if(!onset_descriptor_recipe(plan.recipe,recipe))return false;
    plan.histogram=recipe.kind==OnsetDescriptorKind::Histogram;plan.intermediate=plan.histogram?plan.bins:0;
    plan.bands=plan.histogram?recipe.histogram.histogram.bins:0;
    return PreparedOnsetDescriptor::storage_bytes(plan.bins,plan.recipe,plan.kernel_bytes);
}
bool spectral_plan(const dataflow_runtime::NumericOperationNodeView& node,DescriptorPlan& plan) noexcept {
    std::size_t method=0;plan.kind=DescriptorKind::Spectral;
    if(!integer_parameter(node,"method",0,method) || method>5 ||
       !scalar_parameter(node,"sample_rate",44100,plan.spectral.sample_rate))return false;
    plan.spectral.method=SpectralOnsetMethod(method);plan.phase=method==1 || method==2;
    plan.intermediate=method==4 || plan.phase?plan.bins:0;
    return PreparedSpectralOnset::storage_bytes(plan.bins,plan.spectral,plan.kernel_bytes);
}
bool supported_input(const dataflow_runtime::NumericOperationNodeView& node,const DescriptorPlan& plan,domain::PortId port) noexcept {
    NumericBufferContract contract,magnitude;if(!numeric_port(node,true,port,contract) || !numeric_port(node,true,0,magnitude))return false;
    if(port==descriptor_control_port)return contract.dtype==DType::UInt8 && contract.shape.rank==Rank::Scalar && contract.plane_count==1;
    if(port!=0 && !(port==1 && plan.phase))return false;
    return contract.dtype==DType::Float32 && contract.representation.encoding==StorageEncoding::Native &&
        contract.shape.rank==Rank::Vector && contract.plane_count==1 &&
        contract.shape.extents[0]==ExtentBounds{plan.bins,plan.bins};
}
bool inputs_supported(const dataflow_runtime::NumericOperationNodeView& node,const DescriptorPlan& plan) noexcept {
    if(node.inputs.size()>(plan.phase?3:2))return false;
    NumericBufferContract phase;if(plan.phase && !numeric_port(node,true,1,phase))return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        if(!supported_input(node,plan,node.inputs[i].id))return false;
        if(!operation_port_unique(node.inputs,i))return false;
    }
    return true;
}
}
bool descriptor_operation_kind(std::string_view kind) noexcept {
    return kind=="OnsetDescriptor" || kind=="SpectralOnset";
}
bool plan_descriptor_operation(const dataflow_runtime::NumericOperationNodeView& node,DescriptorPlan& plan) noexcept {
    plan={};if(!descriptor_operation_kind(node.kind) || !vector_capacity(node,domain::PortDirection::Input,0,plan.bins) || !plan.bins)return false;
    bool valid=false;
    if(node.kind=="OnsetDescriptor")valid=onset_plan(node,plan);
    else if(node.kind=="SpectralOnset")valid=spectral_plan(node,plan);
    return valid && inputs_supported(node,plan);
}
std::size_t descriptor_output_count(const DescriptorPlan& plan) noexcept {
    if(plan.histogram)return 3;
    return plan.intermediate?2:1;
}
NumericBufferShape descriptor_output_shape(const DescriptorPlan& plan,domain::PortId port) noexcept {
    NumericBufferShape shape;std::size_t rows=1,columns=1;
    if(port){shape.rank=Rank::Vector;rows=plan.histogram && port==1?plan.bands:plan.intermediate;}
    shape.extents[0]={rows,rows};shape.extents[1]={columns,columns};

    return shape;
}
}
