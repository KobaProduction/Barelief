// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
PcmStageOutputSpec row(std::size_t count,bool variable=false) noexcept {
    return {DType::Float32,Rank::Vector,count,1,variable};
}
PcmStageOutputSpec scalar(DType dtype=DType::Float32) noexcept {return {dtype};}
PcmStageOutputSpec source_ieee(PcmStageOutputSpec spec) noexcept {spec.values=ValueDomain::AnyIeee;return spec;}
PcmStageOutputSpec context_history_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port==11)return source_ieee(row(plan.history/4));
    if(port==12)return source_ieee(row(plan.history));
    if(port==13)return row(plan.history/4+1,true);
    if(port>=9)return row(plan.history);
    return scalar();
}
PcmStageOutputSpec context_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port<2)return scalar(DType::UInt8);
    if(port==6 || port==7)return scalar(DType::Int64);
    if(port==8)return scalar(DType::UInt32);
    if(port==5)return source_ieee(scalar());
    return context_history_spec(plan,port);
}
}
std::size_t pcm_stage_output_count(const PcmStagePlan& plan) noexcept {
    switch(plan.kind){
    case PcmStageKind::Tracker:return 8;
    case PcmStageKind::Sequence:return 14;
    }
    return 0;
}
PcmStageOutputSpec pcm_stage_output_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(plan.kind==PcmStageKind::Sequence)return context_spec(plan,port);
    if(!port)return row(plan.history/4+1,true);
    if(port==4)return scalar(DType::UInt32);
    if(port==3)return source_ieee(scalar());
    if(port==7)return source_ieee(row(plan.history));
    if(port==5)return row(plan.history);
    return port==6?source_ieee(row(plan.history/4)):scalar();
}
}
