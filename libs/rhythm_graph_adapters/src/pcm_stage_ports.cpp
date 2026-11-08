// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
bool scalar_input(const NumericBufferContract& contract,DType dtype) noexcept {
    return contract.dtype==dtype && contract.shape.rank==Rank::Scalar && contract.plane_count==1 &&
        contract.representation.normalization==Normalization::None;
}
bool source_input(const NumericBufferContract& input,const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(plan.kind==PcmStageKind::Sequence && port==context_sequence_ports::silence)return scalar_input(input,DType::UInt8);
    return input.dtype==DType::Float32 && input.shape.rank==Rank::Vector && input.plane_count==1 &&
        input.representation.encoding==StorageEncoding::Native && input.shape.extents[0]==ExtentBounds{plan.input_rows,plan.input_rows};
}
bool warm_scalar(const NumericBufferContract& input,domain::PortId port) noexcept {
    if(port==context_sequence_ports::delay_control)return scalar_input(input,DType::Int32);
    if(port==context_sequence_ports::signature_control)return scalar_input(input,DType::UInt32);
    return port==context_sequence_ports::poll_control && scalar_input(input,DType::UInt8);
}
bool warm_input(const NumericBufferContract& input,const PcmStagePlan& plan,domain::PortId port) noexcept {
    return plan.kind==PcmStageKind::Sequence && warm_scalar(input,port);
}
bool supported_port(const dataflow_runtime::NumericOperationNodeView& node,const PcmStagePlan& plan,domain::PortId id) noexcept {
    NumericBufferContract input;if(!numeric_port(node,true,id,input))return false;
    if(id==pcm_control_port)return scalar_input(input,DType::UInt8);
    if(id<(plan.kind==PcmStageKind::Sequence?4:2))return source_input(input,plan,id);
    return warm_input(input,plan,id);
}
bool supported_ports(const dataflow_runtime::NumericOperationNodeView& node,const PcmStagePlan& plan) noexcept {
    for(std::size_t i=0;i<node.inputs.size();++i){
        const auto id=node.inputs[i].id;
        if(!operation_port_unique(node.inputs,i))return false;
        if(!supported_port(node,plan,id))return false;
    }
    return true;
}

}
bool pcm_stage_inputs(const dataflow_runtime::NumericOperationNodeView& node,const PcmStagePlan& plan) noexcept {
    if(node.inputs.size()>10)return false;
    if(!supported_ports(node,plan))return false;
    const domain::PortId required=plan.kind==PcmStageKind::Sequence?4:2;
    for(domain::PortId port=0;port<required;++port){NumericBufferContract source;if(!numeric_port(node,true,port,source))return false;}
    return true;
}
}
