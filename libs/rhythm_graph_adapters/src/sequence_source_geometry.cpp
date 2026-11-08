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
    if(port==2 || port==3)return row(p.maximum-1);
    if(port==4)return row(250,DType::Float32,false);
    if(port==5)return row(2,DType::Float32,false,3);
    return {DType::UInt64};
}
SequenceSourceShape difference_shape(const SequenceSourcePlan& p,domain::PortId port) noexcept {
    if(port<2){auto shape=row(p.frames,DType::Float32,true,p.features);shape.rank=Rank::Matrix;return shape;}
    return {DType::UInt64};
}
bool sequence_flag_port(SequenceSourceKind kind,domain::PortId port) noexcept {
    return (kind==SequenceSourceKind::Selection && (port==0 || port==4)) || (kind==SequenceSourceKind::Events && (port==0 || port==6));
}
}
std::size_t sequence_source_output_count(SequenceSourceKind kind) noexcept {
    switch(kind){case SequenceSourceKind::Fusion:return 2;case SequenceSourceKind::Selection:return 5;
    case SequenceSourceKind::Events:return 7;
    case SequenceSourceKind::IntervalSummary:return 7;
    case SequenceSourceKind::Activation:case SequenceSourceKind::Difference:return 3;
    }
    return 0;
}
SequenceSourceShape sequence_source_shape(const SequenceSourcePlan& p,domain::PortId port) noexcept {
    if(p.kind==SequenceSourceKind::IntervalSummary)return summary_shape(p,port);
    if(p.kind==SequenceSourceKind::Difference)return difference_shape(p,port);
    if(p.kind==SequenceSourceKind::Activation)return !port?row(p.events):SequenceSourceShape{port==1?DType::Int64:DType::UInt64};
    if(p.kind==SequenceSourceKind::Fusion)return row(port?p.features:p.frames);
    const bool flag=sequence_flag_port(p.kind,port);
    return row(p.frames,flag?DType::UInt8:DType::Float32);
}
}
