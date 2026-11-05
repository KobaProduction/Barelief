// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
bool scalar_input(const NumericContract& contract,DType dtype) noexcept {
    return contract.dtype==dtype && contract.shape.rank==Rank::Scalar && contract.channel_order.size()==1 &&
        contract.representation.normalization==Normalization::None;
}
bool source_input(const NumericContract& input,const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(plan.kind==PcmStageKind::Sequence)return scalar_input(input,port==0?DType::Float32:DType::UInt8);
    return port==0 && input.dtype==DType::Float32 && input.shape.rank==Rank::Vector && input.channel_order.size()==1 &&
        input.representation.encoding==StorageEncoding::Native && input.shape.extents[0]==ExtentBounds{plan.input_rows,plan.input_rows} &&
        (plan.kind==PcmStageKind::Tracker || !input.clock.sample_rate || input.clock.sample_rate==plan.sample_rate);
}
bool spectrum_control(const NumericContract& input,const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port==17)return scalar_input(input,DType::UInt32);
    return port==18 && input.dtype==DType::Float32 && input.shape.rank==Rank::Vector && input.channel_order.size()==1 &&
        input.representation.encoding==StorageEncoding::Native && input.shape.extents[0]==ExtentBounds{plan.onset.spectrum.window_samples,plan.onset.spectrum.window_samples};
}
bool warm_input(const NumericContract& input,const PcmStagePlan& plan,domain::PortId port) noexcept {
    const bool onset=plan.kind==PcmStageKind::Onset,sequence=plan.kind==PcmStageKind::Sequence;
    if(plan.kind==PcmStageKind::Spectrum)return spectrum_control(input,plan,port);
    if(plan.kind==PcmStageKind::Tracker)return false;
    if((port==10 || port==11) && !sequence)return scalar_input(input,DType::Float32);
    if(port==12)return onset && scalar_input(input,DType::Float32);
    if(port==13)return onset && scalar_input(input,DType::UInt8);
    if(port==14)return scalar_input(input,onset?DType::UInt64:DType::Int32);
    if(port==15)return scalar_input(input,onset?DType::UInt64:DType::UInt32);
    return port==16 && scalar_input(input,DType::UInt8);
}
bool supported_port(const domain::ResolvedProcessingNode& node,const PcmStagePlan& plan,domain::PortId id) noexcept {
    NumericContract input;if(!numeric_port(node,true,id,input))return false;
    if(id==pcm_control_port)return scalar_input(input,DType::UInt8);
    if(id==0 || (id==1 && plan.kind==PcmStageKind::Sequence))return source_input(input,plan,id);
    return warm_input(input,plan,id);
}
}
bool pcm_stage_inputs(const domain::ResolvedProcessingNode& node,const PcmStagePlan& plan) noexcept {
    if(node.inputs.size()>10)return false;
    NumericContract source;if(!numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<node.inputs.size();++i){
        const auto id=node.inputs[i].id;
        for(std::size_t j=0;j<i;++j)if(node.inputs[j].id==id)return false;
        if(!supported_port(node,plan,id))return false;
    }
    if(plan.kind!=PcmStageKind::Sequence)return true;
    NumericContract silent;return numeric_port(node,true,1,silent) && equal_frame_clock(source.clock,silent.clock);
}
}
