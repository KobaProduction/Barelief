// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include "frame_operation_io.h"
#include <cmath>
#include <cstdint>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
struct Kind {std::string_view name;BatchKind kind;};
constexpr Kind kinds[]{{"PulseTrainScoring",BatchKind::PulseTrain},
    {"LagTempoConsensus",BatchKind::LagConsensus},{"FourierTempoCandidates",BatchKind::FourierCandidates},
    {"WeightedTempoHistogram",BatchKind::WeightedHistogram},{"FrameTempoPath",BatchKind::FramePath},};
bool dimensions(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId id,Rank rank,std::size_t& a,std::size_t& b,DType dtype=DType::Float32) noexcept {
    NumericBufferContract c;if(!numeric_port(n,true,id,c) || c.dtype!=dtype || c.shape.rank!=rank || c.plane_count!=1 || c.representation.encoding!=StorageEncoding::Native || c.representation.valid_bits || c.representation.layout!=ChannelLayout::Planar)return false;
    if(c.shape.extents[0].maximum>SIZE_MAX || c.shape.extents[1].maximum>SIZE_MAX)return false;
    a=std::size_t(c.shape.extents[0].maximum);b=std::size_t(c.shape.extents[1].maximum);return a && b;
}
void classifier_row(BatchPlan& p,std::size_t i,std::span<const float> minimum,std::span<const float> maximum,std::span<const float> weights) noexcept {
    if(!minimum.empty())p.classifier.minimum[i]=minimum[i];
    if(!maximum.empty())p.classifier.maximum[i]=maximum[i];
    if(!weights.empty())for(std::size_t j=0;j<4;++j)p.classifier.weights[i][j]=weights[4*i+j];
}
bool classifier_config(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    const auto minimum=array_parameter(n,"classifier_minimum"),maximum=array_parameter(n,"classifier_maximum"),weights=array_parameter(n,"classifier_weights");
    if((!minimum.empty() && minimum.size()!=3) || (!maximum.empty() && maximum.size()!=3) || (!weights.empty() && weights.size()!=12))return false;
    for(std::size_t i=0;i<3;++i)classifier_row(p,i,minimum,maximum,weights);
    return true;
}
bool path_config(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    auto& c=p.histogram.path;
    return scalar_parameter(n,"frequency_step",1,c.frequency_step) && scalar_parameter(n,"frame_rate",44100.f/512.f,c.frame_rate) &&
        scalar_parameter(n,"minimum_bpm",30,c.minimum_bpm) && scalar_parameter(n,"maximum_bpm",560,c.maximum_bpm) && scalar_parameter(n,"tolerance_percent",3,c.tolerance_percent) &&
        scalar_parameter(n,"minimum_change_seconds",5,c.minimum_change_seconds) && scalar_parameter(n,"induced_bpm",0,c.induced_bpm) && integer_parameter(n,"hop",32,c.hop) &&
        batch_bool(n,"constant_tempo",false,c.constant_tempo) && integer_parameter(n,"maximum_peaks",50,p.histogram.maximum_peaks) && batch_bool(n,"weight_by_magnitude",true,p.histogram.weight_by_magnitude);
}
bool secondary_geometry(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    std::size_t a=0,b=0;
    if(p.kind==BatchKind::PulseTrain)return dimensions(n,1,Rank::Vector,p.secondary,b);
    if(p.kind==BatchKind::WeightedHistogram){
        const auto maximum=std::ceil(p.histogram.path.maximum_bpm);
        return maximum>=0 && double(maximum)<double(SIZE_MAX/sizeof(float))-2 && p.columns==2 && dimensions(n,1,Rank::Matrix,p.secondary,b) && b==std::size_t(maximum)+1;
    }
    if(p.kind==BatchKind::FramePath)return p.columns%2==0 && dimensions(n,1,Rank::Vector,a,b,DType::UInt64) && a==p.rows && dimensions(n,2,Rank::Scalar,a,b);
    return true;
}
bool geometry(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    const bool matrix=p.kind==BatchKind::WeightedHistogram || p.kind==BatchKind::FramePath;
    if(!dimensions(n,0,matrix?Rank::Matrix:Rank::Vector,p.rows,p.columns,p.kind==BatchKind::LagConsensus?DType::Int32:DType::Float32))return false;
    p.frames=p.rows;p.bins=p.columns;
    return secondary_geometry(n,p);
}

}
bool batch_bool(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key,bool fallback,bool& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}const auto* v=std::get_if<bool>(p);if(!v)return false;out=*v;return true;
}
bool batch_operation_kind(std::string_view id) noexcept {for(const auto& k:kinds)if(k.name==id)return true;return false;}
namespace {
bool select_batch_kind(std::string_view kind,BatchPlan& p) noexcept {
    for(const auto& item:kinds)if(item.name==kind){p.kind=item.kind;return true;}
    return false;
}
bool batch_settings(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    if(p.kind==BatchKind::LagConsensus)return classifier_config(n,p);
    return p.kind==BatchKind::PulseTrain || path_config(n,p);
}
bool batch_frame_rate(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    return p.kind!=BatchKind::LagConsensus || scalar_parameter(n,"onset_frame_rate",44100.f/128.f,p.rate);
}
}
bool batch_operation_plan(const dataflow_runtime::NumericOperationNodeView& n,BatchPlan& p) noexcept {
    return select_batch_kind(n.kind,p) && batch_settings(n,p) && batch_frame_rate(n,p) && geometry(n,p);
}
}
