// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
SequenceSourceShape row(std::size_t count,DType dtype=DType::Float32,bool variable=true,std::size_t columns=1) noexcept {
    return {dtype,columns==1?Rank::Vector:Rank::Matrix,std::max(std::size_t{1},count),columns,variable};
}
SequenceSourceShape summary_shape(const SequenceSourcePlan& p,domain::PortId port) noexcept {
    if(port<2)return {};
    if(p.kind==SequenceSourceKind::PcmSummary){
        if(port==2)return row(p.events);
        if(port<5)return row(p.events?p.events-1:0);
        return port==5?row(250,DType::Float32,false):row(2,DType::Float32,false,3);
    }
    if(port==2 || port==3)return row(p.maximum-1);
    if(port==4)return row(250,DType::Float32,false);
    if(port==5)return row(2,DType::Float32,false,3);
    return {DType::UInt64};
}
SequenceSourceShape trajectory_shape(const SequenceSourcePlan& p,domain::PortId port) noexcept {
    if(!port)return row(p.events);
    if(port==1)return row(p.frames>2?p.frames-2:0);
    if(port==2)return row(p.frames,DType::Float32,true,p.features);
    if(port==3)return row(p.bins,DType::Float32,true,2);
    if(port==4)return row(p.bins);
    if(port==5)return row(p.features,DType::Float32,false);
    if(port==6)return {DType::Int64};
    if(port==7 || port==8)return {DType::UInt64};
    return row(p.trajectory.frame_size);
}
SequenceSourceShape difference_shape(const SequenceSourcePlan& p,domain::PortId port) noexcept {
    if(port<2){auto shape=row(p.frames,DType::Float32,true,p.features);shape.rank=Rank::Matrix;return shape;}
    if(p.kind==SequenceSourceKind::Difference)return {DType::UInt64};
    if(port==2)return row(p.bins+1,DType::Float32,true,2);
    if(port==3 || port==4)return row(p.bins);
    if(port==5 || port==6)return row(p.features);
    if(port==7)return row(p.pcm_difference.spectrum.framing.length);
    return row(p.features);
}
}
std::size_t sequence_source_output_count(SequenceSourceKind kind) noexcept {
    switch(kind){case SequenceSourceKind::Fusion:return 2;case SequenceSourceKind::Selection:return 5;
    case SequenceSourceKind::Events:return 7;case SequenceSourceKind::Rate:return 10;
    case SequenceSourceKind::IntervalSummary:case SequenceSourceKind::PcmSummary:return 7;
    case SequenceSourceKind::Activation:case SequenceSourceKind::Difference:return 3;
    case SequenceSourceKind::PcmDifference:return 9;case SequenceSourceKind::Trajectory:return 11;}
    return 0;
}
SequenceSourceShape sequence_source_shape(const SequenceSourcePlan& p,domain::PortId port) noexcept {
    if(p.kind==SequenceSourceKind::Trajectory)return trajectory_shape(p,port);
    if(p.kind==SequenceSourceKind::IntervalSummary || p.kind==SequenceSourceKind::PcmSummary)return summary_shape(p,port);
    if(p.kind==SequenceSourceKind::Difference || p.kind==SequenceSourceKind::PcmDifference)return difference_shape(p,port);
    if(p.kind==SequenceSourceKind::Activation)return !port?row(p.events):SequenceSourceShape{port==1?DType::Int64:DType::UInt64};
    if(p.kind==SequenceSourceKind::Rate)return port<2?SequenceSourceShape{}:row(p.frames,port==9?DType::UInt8:DType::Float32);
    if(p.kind==SequenceSourceKind::Fusion)return row(port?p.features:p.frames);
    const bool flag=(p.kind==SequenceSourceKind::Selection && (port==0 || port==4)) || (p.kind==SequenceSourceKind::Events && (port==0 || port==6));
    return row(p.frames,flag?DType::UInt8:DType::Float32);
}
}
