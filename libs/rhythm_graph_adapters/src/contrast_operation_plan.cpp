// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool settings(const dataflow_runtime::NumericOperationNodeView& node,ContrastPeakPickerConfig& config) noexcept {
    return scalar_parameter(node,"frame_rate",172,config.frame_rate) && scalar_parameter(node,"threshold",.05f,config.threshold) &&
        scalar_parameter(node,"ratio_threshold",16,config.ratio_threshold) && scalar_parameter(node,"combine_ms",30,config.combine_ms) &&
        scalar_parameter(node,"pre_average_ms",100,config.pre_average_ms) && scalar_parameter(node,"pre_maximum_ms",30,config.pre_maximum_ms);
}
bool contrast_input(const NumericBufferContract& input,const ContrastPlan& plan,domain::PortId id) noexcept {
    if(id==contrast_control_port)return input.dtype==DType::UInt8 && input.shape.rank==Rank::Scalar && input.plane_count==1;
    return id==0 && input.dtype==DType::Float32 && input.shape.rank==Rank::Vector && input.plane_count==1 &&
        input.representation.encoding==StorageEncoding::Native && input.shape.extents[0].maximum==plan.maximum;
}
bool input_contracts(const dataflow_runtime::NumericOperationNodeView& node,const ContrastPlan& plan) noexcept {
    if(node.inputs.size()>2)return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        const auto id=node.inputs[i].id;NumericBufferContract input;if(!numeric_port(node,true,id,input))return false;
        if(!operation_port_unique(node.inputs,i) || !contrast_input(input,plan,id))return false;
    }
    return true;
}
}
bool contrast_operation_kind(std::string_view kind) noexcept {
    return kind=="ContrastPeakPicker" || kind=="ContrastPeakAccumulator";
}
bool plan_contrast_operation(const dataflow_runtime::NumericOperationNodeView& node,ContrastPlan& plan) noexcept {
    plan={};if(!contrast_operation_kind(node.kind) || !vector_capacity(node,domain::PortDirection::Input,0,plan.maximum) || !plan.maximum ||
        !settings(node,plan.config) || !input_contracts(node,plan))return false;
    plan.accumulator=node.kind=="ContrastPeakAccumulator";plan.events=plan.maximum;
    if(!plan.accumulator)return PreparedContrastPeakPicker::storage_bytes(plan.maximum,plan.config,plan.kernel_bytes);
    return integer_parameter(node,"maximum_onsets",4096,plan.events) &&
        PreparedContrastPeakAccumulator::storage_bytes(plan.maximum,plan.events,plan.config,plan.kernel_bytes) &&
        dataflow_runtime::numeric_operation_mul_bytes(plan.events,sizeof(float),plan.buffer_bytes);
}
std::size_t contrast_output_count(const ContrastPlan& plan) noexcept {return plan.accumulator?2:4;}
namespace {
bool contrast_output(const dataflow_runtime::NumericOperationNodeView& node,const ContrastPlan& plan,domain::PortId id) noexcept {
    NumericBufferContract output;if(!numeric_port(node,false,id,output))return false;
    const bool counter=plan.accumulator?id==1:id==3;
    if(output.dtype!=(counter?DType::UInt64:DType::Float32) || output.plane_count!=1 ||
       output.representation.normalization!=Normalization::None || output.shape.rank!=(counter?Rank::Scalar:Rank::Vector))return false;
    return counter || (output.shape.extents[0].minimum==0 && output.shape.extents[0].maximum>=(id?plan.maximum:plan.events));
}
}
bool contrast_output_contracts(const dataflow_runtime::NumericOperationNodeView& node,const ContrastPlan& plan) noexcept {
    if(node.outputs.size()!=contrast_output_count(plan))return false;
    NumericBufferContract input;if(!numeric_port(node,true,0,input))return false;
    for(std::size_t i=0;i<node.outputs.size();++i)if(!contrast_output(node,plan,domain::PortId(i)))return false;
    return true;
}
}
