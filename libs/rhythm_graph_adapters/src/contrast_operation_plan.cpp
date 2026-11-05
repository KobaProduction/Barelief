// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool settings(const domain::ResolvedProcessingNode& node,ContrastPeakPickerConfig& config) noexcept {
    return scalar_parameter(node,"frame_rate",172,config.frame_rate) && scalar_parameter(node,"threshold",.05f,config.threshold) &&
        scalar_parameter(node,"ratio_threshold",16,config.ratio_threshold) && scalar_parameter(node,"combine_ms",30,config.combine_ms) &&
        scalar_parameter(node,"pre_average_ms",100,config.pre_average_ms) && scalar_parameter(node,"pre_maximum_ms",30,config.pre_maximum_ms);
}
bool input_contracts(const domain::ResolvedProcessingNode& node,const ContrastPlan& plan) noexcept {
    if(node.inputs.size()>2)return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        const auto id=node.inputs[i].id;NumericContract input;if(!numeric_port(node,true,id,input))return false;
        for(std::size_t j=0;j<i;++j)if(node.inputs[j].id==id)return false;
        if(id==contrast_control_port){if(input.dtype!=DType::UInt8 || input.shape.rank!=Rank::Scalar || input.channel_order.size()!=1)return false;}
        else if(id!=0 || input.dtype!=DType::Float32 || input.shape.rank!=Rank::Vector || input.channel_order.size()!=1 ||
            input.representation.encoding!=StorageEncoding::Native || input.shape.extents[0].maximum!=plan.maximum)return false;
    }
    return true;
}
}
bool contrast_operation_kind(std::string_view kind) noexcept {
    return kind=="ContrastPeakPicker" || kind=="ContrastPeakAccumulator";
}
bool plan_contrast_operation(const domain::ResolvedProcessingNode& node,ContrastPlan& plan) noexcept {
    plan={};if(!contrast_operation_kind(node.kind) || !vector_capacity(node,domain::PortDirection::Input,0,plan.maximum) || !plan.maximum ||
        !settings(node,plan.config) || !input_contracts(node,plan))return false;
    plan.accumulator=node.kind=="ContrastPeakAccumulator";plan.events=plan.maximum;
    if(!plan.accumulator)return PreparedContrastPeakPicker::storage_bytes(plan.maximum,plan.config,plan.kernel_bytes);
    return integer_parameter(node,"maximum_onsets",4096,plan.events) &&
        PreparedContrastPeakAccumulator::storage_bytes(plan.maximum,plan.events,plan.config,plan.kernel_bytes) &&
        dataflow_runtime::numeric_operation_mul_bytes(plan.events,sizeof(float),plan.buffer_bytes);
}
std::size_t contrast_output_count(const ContrastPlan& plan) noexcept {return plan.accumulator?2:4;}
bool contrast_output_contracts(const domain::ResolvedProcessingNode& node,const ContrastPlan& plan) noexcept {
    if(node.outputs.size()!=contrast_output_count(plan))return false;
    NumericContract input;if(!numeric_port(node,true,0,input))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericContract output;if(!numeric_port(node,false,domain::PortId(i),output))return false;
        const bool counter=plan.accumulator?i==1:i==3;
        if(output.dtype!=(counter?DType::UInt64:DType::Float32) || output.channel_order.size()!=1 ||
           output.representation.normalization!=Normalization::None || output.shape.rank!=(counter?Rank::Scalar:Rank::Vector))return false;
        if(i && !equal_frame_clock(output.clock,input.clock))return false;
        if(!i && !index_output_contract(output))return false;
        output.clock={};if(!index_output_contract(output))return false;
        if(!counter && (output.shape.extents[0].minimum!=0 || output.shape.extents[0].maximum<(i?plan.maximum:plan.events)))return false;
    }
    return true;
}
}
