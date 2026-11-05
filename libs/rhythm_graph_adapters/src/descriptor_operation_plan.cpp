// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool flag(const domain::ResolvedProcessingNode& node,std::string_view key,bool fallback,bool& value) noexcept {
    const auto* setting=parameter(node,key);if(!setting){value=fallback;return true;}
    const auto* boolean=std::get_if<bool>(setting);if(!boolean)return false;
    value=*boolean;return true;
}
bool triangle_plan(const domain::ResolvedProcessingNode& node,DescriptorPlan& plan) noexcept {
    plan.kind=DescriptorKind::Bands;std::size_t warp=0,normalization=0;
    if(!scalar_parameter(node,"sample_rate",44100,plan.triangular.sample_rate) ||
       !integer_parameter(node,"weighting",0,warp) || warp>2 ||
       !integer_parameter(node,"normalization",0,normalization) || normalization>2 ||
       !flag(node,"square_magnitude",true,plan.triangular.square_magnitude) ||
       !flag(node,"logarithm",true,plan.triangular.logarithm))return false;
    plan.triangular.weighting=TriangleWarp(warp);plan.triangular.normalization=TriangleNormalization(normalization);
    const auto edges=array_parameter(node,"edges_hz");
    if(!PreparedTriangularSpectralBands::storage_bytes(plan.bins,edges,plan.triangular,plan.kernel_bytes))return false;
    plan.bands=edges.size()-2;
    return dataflow_runtime::numeric_operation_mul_bytes(plan.bands,sizeof(float),plan.buffer_bytes);
}
bool onset_plan(const domain::ResolvedProcessingNode& node,DescriptorPlan& plan) noexcept {
    std::size_t method=1;OnsetDescriptorRecipe recipe;
    if(!integer_parameter(node,"method",1,method) || method>9)return false;
    plan.kind=DescriptorKind::Onset;plan.recipe=OnsetRecipe(method);
    plan.phase=plan.recipe==OnsetRecipe::Complex || plan.recipe==OnsetRecipe::Phase || plan.recipe==OnsetRecipe::WeightedPhase;
    if(!onset_descriptor_recipe(plan.recipe,recipe))return false;
    plan.histogram=recipe.kind==OnsetDescriptorKind::Histogram;plan.intermediate=plan.histogram?plan.bins:0;
    plan.bands=plan.histogram?recipe.histogram.histogram.bins:0;
    return PreparedOnsetDescriptor::storage_bytes(plan.bins,plan.recipe,plan.kernel_bytes);
}
bool spectral_plan(const domain::ResolvedProcessingNode& node,DescriptorPlan& plan) noexcept {
    std::size_t method=0;plan.kind=DescriptorKind::Spectral;
    if(!integer_parameter(node,"method",0,method) || method>5 ||
       !scalar_parameter(node,"sample_rate",44100,plan.spectral.sample_rate))return false;
    plan.spectral.method=SpectralOnsetMethod(method);plan.phase=method==1 || method==2;
    plan.intermediate=method==4?40:plan.phase?plan.bins:0;
    return PreparedSpectralOnset::storage_bytes(plan.bins,plan.spectral,plan.kernel_bytes);
}
bool supported_input(const domain::ResolvedProcessingNode& node,const DescriptorPlan& plan,domain::PortId port) noexcept {
    NumericContract contract,magnitude;if(!numeric_port(node,true,port,contract) || !numeric_port(node,true,0,magnitude))return false;
    if(port==descriptor_control_port)return contract.dtype==DType::UInt8 && contract.shape.rank==Rank::Scalar && contract.channel_order.size()==1;
    if(port!=0 && !(port==1 && plan.phase))return false;
    return contract.dtype==DType::Float32 && contract.representation.encoding==StorageEncoding::Native &&
        contract.shape.rank==Rank::Vector && contract.channel_order.size()==1 &&
        contract.shape.extents[0]==ExtentBounds{plan.bins,plan.bins} &&
        equal_frame_clock(contract.clock,magnitude.clock) && contract.coordinates==magnitude.coordinates;
}
bool inputs_supported(const domain::ResolvedProcessingNode& node,const DescriptorPlan& plan) noexcept {
    if(node.inputs.size()>(plan.phase?3:2))return false;
    NumericContract phase;if(plan.phase && !numeric_port(node,true,1,phase))return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        if(!supported_input(node,plan,node.inputs[i].id))return false;
        for(std::size_t j=0;j<i;++j)if(node.inputs[i].id==node.inputs[j].id)return false;
    }
    return true;
}
}
bool descriptor_operation_kind(std::string_view kind) noexcept {
    return kind=="OnsetDescriptor" || kind=="SpectralOnset" || kind=="TriangularSpectralBands";
}
bool plan_descriptor_operation(const domain::ResolvedProcessingNode& node,DescriptorPlan& plan) noexcept {
    plan={};if(!descriptor_operation_kind(node.kind) || !vector_capacity(node,domain::PortDirection::Input,0,plan.bins) || !plan.bins)return false;
    bool valid=false;
    if(node.kind=="OnsetDescriptor")valid=onset_plan(node,plan);
    else if(node.kind=="SpectralOnset")valid=spectral_plan(node,plan);
    else valid=triangle_plan(node,plan);
    return valid && inputs_supported(node,plan);
}
std::size_t descriptor_output_count(const DescriptorPlan& plan) noexcept {
    if(plan.kind==DescriptorKind::Bands || plan.histogram)return 3;
    return plan.intermediate?2:1;
}
ShapeContract descriptor_output_shape(const DescriptorPlan& plan,domain::PortId port) noexcept {
    ShapeContract shape;std::size_t rows=1,columns=1;
    if(plan.kind==DescriptorKind::Bands){
        rows=plan.bands;columns=port==2?plan.bins:1;shape.rank=port==2?Rank::Matrix:Rank::Vector;
    }else if(port){shape.rank=Rank::Vector;rows=plan.histogram && port==1?plan.bands:plan.intermediate;}
    shape.extents[0]={rows,rows};shape.extents[1]={columns,columns};
    if(shape.rank!=Rank::Scalar)shape.axes[0]=Axis::Declared;
    if(shape.rank==Rank::Matrix)shape.axes[1]=Axis::Declared;
    return shape;
}
}
