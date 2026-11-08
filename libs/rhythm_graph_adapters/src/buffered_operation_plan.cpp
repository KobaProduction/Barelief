// SPDX-License-Identifier: AGPL-3.0-or-later
#include "buffered_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
namespace {
bool kernel_plan(const dataflow_runtime::NumericOperationNodeView& node,BufferedPlan& plan) noexcept {
    if(node.kind=="BufferedBandFlux"){
        plan.kind=BufferedOperationKind::Flux;
        return integer_parameter(node,"bin_width",3,plan.flux.bin_width) && integer_parameter(node,"frame_width",2,plan.flux.frame_width) &&
            integer_parameter(node,"maximum_frames",3,plan.maximum_frames) &&
            PreparedBufferedBandFlux::storage_bytes(plan.maximum,plan.maximum_frames,plan.flux,plan.kernel_bytes);
    }
    if(node.kind=="CausalAverage"){
        plan.kind=BufferedOperationKind::Average;
        return integer_parameter(node,"length",5,plan.width) && PreparedCausalAverage::storage_bytes(plan.width,plan.kernel_bytes);
    }
    const auto* setting=parameter(node,"causal");
    if(setting){const auto* flag=std::get_if<bool>(setting);if(!flag)return false;plan.causal=*flag;}
    return integer_parameter(node,"width",3,plan.width) && PreparedBufferedMaximum::storage_bytes(plan.width,plan.kernel_bytes);
}
bool supported_port(const dataflow_runtime::NumericOperationNodeView& node,const BufferedPlan& plan,domain::PortId port) noexcept {
    NumericBufferContract input,current;if(!numeric_port(node,true,port,input) || !numeric_port(node,true,0,current))return false;
    if(port==buffered_control_port)return input.dtype==DType::UInt8 && input.shape.rank==Rank::Scalar && input.plane_count==1;
    if(port!=0 && !(port==1 && plan.kind==BufferedOperationKind::Flux))return false;
    if(input.dtype!=DType::Float32 || input.shape.rank!=Rank::Vector || input.plane_count!=1 ||
       input.representation.encoding!=StorageEncoding::Native || input.shape.extents[0].maximum!=plan.maximum || input.shape.extents[0].minimum<1)return false;
    if(plan.kind!=BufferedOperationKind::Flux)return true;
    return input.shape.extents[0].minimum==plan.maximum;
}
bool input_contracts(const dataflow_runtime::NumericOperationNodeView& node,const BufferedPlan& plan) noexcept {
    if(node.inputs.size()>(plan.kind==BufferedOperationKind::Flux?3:2))return false;
    NumericBufferContract delayed;if(plan.kind==BufferedOperationKind::Flux && !numeric_port(node,true,1,delayed))return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        if(!supported_port(node,plan,node.inputs[i].id))return false;
        if(!operation_port_unique(node.inputs,i))return false;
    }
    return true;
}
}
bool buffered_operation_kind(std::string_view kind) noexcept {
    return kind=="BufferedMaximum" || kind=="CausalAverage" || kind=="BufferedBandFlux";
}
bool plan_buffered_operation(const dataflow_runtime::NumericOperationNodeView& node,BufferedPlan& plan) noexcept {
    plan={};if(!buffered_operation_kind(node.kind) || !vector_capacity(node,domain::PortDirection::Input,0,plan.maximum) || !plan.maximum ||
        !kernel_plan(node,plan) || !input_contracts(node,plan))return false;
    return plan.kind==BufferedOperationKind::Flux || dataflow_runtime::numeric_operation_mul_bytes(plan.maximum,sizeof(float),plan.buffer_bytes);
}
}
