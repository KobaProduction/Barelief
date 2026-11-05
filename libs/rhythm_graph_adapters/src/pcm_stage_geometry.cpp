// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
PcmStageOutputSpec row(std::size_t count,PcmStageAxis axis=PcmStageAxis::Index,bool variable=false) noexcept {
    return {DType::Float32,Rank::Vector,count,1,variable,axis};
}
PcmStageOutputSpec scalar(DType dtype=DType::Float32) noexcept {return {dtype};}
PcmStageOutputSpec source_ieee(PcmStageOutputSpec spec) noexcept {spec.values=ValueDomain::AnyIeee;return spec;}
PcmStageOutputSpec spectrum_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port<2)return row(plan.bins,PcmStageAxis::Frequency);
    if(port==2)return {DType::Float32,Rank::Matrix,plan.bins,2,false,PcmStageAxis::Frequency};
    if(port==7)return scalar(DType::Int64);
    return row(plan.onset.spectrum.window_samples,port==3 || port==5?PcmStageAxis::LocalTime:PcmStageAxis::Index);
}
PcmStageOutputSpec context_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port<2)return scalar(DType::UInt8);
    if(port==6 || port==7)return scalar(DType::Int64);
    if(port==8)return scalar(DType::UInt32);
    if(port==5)return source_ieee(scalar());
    if(port==11)return source_ieee(row(plan.history/4));
    if(port==12)return source_ieee(row(plan.history));
    if(port==13)return row(plan.history/4+1,PcmStageAxis::Index,true);
    if(port>=9)return row(plan.history);
    return scalar();
}
PcmStageOutputSpec onset_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port==5 || port==7)return scalar(DType::UInt8);
    if(port==6)return scalar(DType::UInt64);
    if(port<8)return scalar();
    if(port==10)return row(plan.bins,PcmStageAxis::Frequency);
    return spectrum_spec(plan,port<10?domain::PortId(port-8):domain::PortId(port-9));
}
PcmStageOutputSpec tempo_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(port==3)return scalar(DType::UInt8);
    if(port==8 || port==9)return scalar(DType::Int64);
    if(port==10)return scalar(DType::UInt32);
    if(port==7)return source_ieee(scalar());
    if(port<11)return scalar();
    if(port==11 || port==12)return row(plan.bins,PcmStageAxis::Frequency);
    if(port>=18)return spectrum_spec(plan,domain::PortId(port-16));
    return context_spec(plan,domain::PortId(port-4));
}
}
std::size_t pcm_stage_output_count(const PcmStagePlan& plan) noexcept {
    switch(plan.kind){
    case PcmStageKind::Spectrum:return 8;
    case PcmStageKind::Onset:return 17;
    case PcmStageKind::Tempo:return 24;
    case PcmStageKind::Tracker:return 8;
    case PcmStageKind::Sequence:return 14;
    }
    return 0;
}
PcmStageOutputSpec pcm_stage_output_spec(const PcmStagePlan& plan,domain::PortId port) noexcept {
    if(plan.kind==PcmStageKind::Spectrum)return spectrum_spec(plan,port);
    if(plan.kind==PcmStageKind::Onset)return onset_spec(plan,port);
    if(plan.kind==PcmStageKind::Tempo)return tempo_spec(plan,port);
    if(plan.kind==PcmStageKind::Sequence)return context_spec(plan,port);
    if(!port)return row(plan.history/4+1,PcmStageAxis::Index,true);
    if(port==4)return scalar(DType::UInt32);
    if(port==3)return source_ieee(scalar());
    if(port==7)return source_ieee(row(plan.history));
    if(port==5)return row(plan.history);
    return port==6?source_ieee(row(plan.history/4)):scalar();
}
}
