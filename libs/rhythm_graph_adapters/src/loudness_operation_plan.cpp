// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
#include "frame_operation_io.h"
#include <cmath>
#include <climits>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
struct Entry {std::string_view id;LoudnessKind kind;};
constexpr Entry entries[]{{"LoopLengthFit",LoudnessKind::LoopFit},{"LoopPcmTempo",LoudnessKind::LoopTempo},
    {"BeatSegmentEnergy",LoudnessKind::Segment},{"SpectralBandRatios",LoudnessKind::Ratios},{"FrequencyBandEnergy",LoudnessKind::Frequency},
    {"BeatLoudnessSequence",LoudnessKind::Sequence},{"BeatBandSalience",LoudnessKind::Salience},{"CorrelationMeter",LoudnessKind::Meter},
    {"BandRhythmTransform",LoudnessKind::Transform},{"ActivityFluctuation",LoudnessKind::Activity}};
bool dimensions(const domain::ResolvedProcessingNode& n,domain::PortId id,Rank rank,std::size_t& a,std::size_t& b) noexcept {
    NumericContract c;if(!numeric_port(n,true,id,c) || c.dtype!=DType::Float32 || c.shape.rank!=rank || c.channel_order.size()!=1 ||
        c.representation.encoding!=StorageEncoding::Native || c.representation.layout!=ChannelLayout::Planar || c.representation.valid_bits ||
        (c.representation.normalization!=Normalization::None && c.representation.normalization!=Normalization::FloatUnit))return false;
    if(c.shape.extents[0].maximum>INT_MAX || c.shape.extents[1].maximum>INT_MAX)return false;
    a=std::size_t(c.shape.extents[0].maximum);b=std::size_t(c.shape.extents[1].maximum);return a && b;
}
bool pcm_clock(const domain::ResolvedProcessingNode& n,const LoudnessPlan& p) noexcept {
    if(p.kind!=LoudnessKind::LoopFit && p.kind!=LoudnessKind::LoopTempo && p.kind!=LoudnessKind::Segment && p.kind!=LoudnessKind::Sequence && p.kind!=LoudnessKind::Activity)return true;
    NumericContract c;if(!numeric_port(n,true,0,c))return false;
    return !c.clock.sample_rate || double(c.clock.sample_rate)==double(p.rate);
}
bool settings(const domain::ResolvedProcessingNode& n,LoudnessPlan& p) noexcept {
    std::size_t alignment=0;
    if(!scalar_parameter(n,"sample_rate",44100,p.rate) || !std::isfinite(p.rate) || p.rate<=0 || !scalar_parameter(n,"confidence_threshold",.95f,p.threshold) ||
       !scalar_parameter(n,"search_seconds",.1f,p.segment.search_seconds) || !scalar_parameter(n,"energy_seconds",.05f,p.segment.energy_seconds) ||
       !integer_parameter(n,"alignment",0,alignment) || alignment>1 || !integer_parameter(n,"window_size",16,p.window) ||
       !scalar_parameter(n,"frame_rate",120,p.analysis_rate) || !std::isfinite(p.analysis_rate) || p.analysis_rate<=0 || !integer_parameter(n,"frame_size",256,p.transform.frame_size) || !integer_parameter(n,"hop",32,p.transform.hop))return false;
    p.segment.sample_rate=p.rate;p.segment.alignment=BeatOnsetAlignment(alignment);p.sequence={p.rate,p.segment.search_seconds,p.segment.energy_seconds};p.activity.sample_rate=p.rate;
    return scalar_parameter(n,"minimum_tau_ms",310,p.activity.minimum_tau_ms) && scalar_parameter(n,"maximum_tau_ms",8800,p.activity.maximum_tau_ms) &&
        scalar_parameter(n,"tau_multiplier",1.1f,p.activity.tau_multiplier);
}
BatchShape scalar(DType type=DType::Float32) noexcept {return {Rank::Scalar,1,1,type};}
BatchShape vector(std::size_t n,bool variable=false,DType type=DType::Float32) noexcept {return {Rank::Vector,std::max(std::size_t{1},n),1,type,variable};}
BatchShape matrix(std::size_t a,std::size_t b,bool variable=false) noexcept {return {Rank::Matrix,std::max(std::size_t{1},a),b,DType::Float32,variable};}
void loop_shapes(LoudnessPlan& p) noexcept {
    p.outputs=p.kind==LoudnessKind::LoopFit?6:9;
    for(std::size_t i=0;i<p.outputs;++i)p.shape[i]=scalar();
    if(p.kind==LoudnessKind::LoopFit){p.shape[2]=p.shape[3]=scalar(DType::UInt64);p.shape[4]=vector(4,false,DType::Int64);p.shape[5]=vector(4);}
    else {p.shape[4]=p.shape[5]=scalar(DType::UInt64);p.shape[6]=vector(4,false,DType::Int64);p.shape[7]=vector(4);p.shape[8]=vector(414);}
}
bool energy_shapes(const domain::ResolvedProcessingNode& n,LoudnessPlan& p) noexcept {
    const auto borders=loudness_borders(n,p.kind);if(borders.size()<2)return false;p.bands=borders.size()-1;
    if(p.kind==LoudnessKind::Segment){
        const float length=p.segment.energy_seconds*std::trunc(p.rate);if(!std::isfinite(length) || length<1 || length>INT_MAX-2)return false;
        const auto size=std::size_t(length);p.bins=(size+size%2)/2+1;p.outputs=4;p.shape[0]=scalar();p.shape[1]=vector(p.bands);p.shape[2]=scalar(DType::UInt64);p.shape[3]=matrix(p.bins,2,true);return true;
    }
    if(p.kind==LoudnessKind::Sequence){p.outputs=3;p.shape[0]=vector(p.secondary,true);p.shape[1]=matrix(p.secondary,p.bands,true);p.shape[2]=vector(p.secondary,true,DType::UInt64);return true;}
    p.outputs=p.kind==LoudnessKind::Ratios?2:1;p.shape[0]=vector(p.bands);if(p.outputs==2){p.shape[0]=scalar();p.shape[1]=vector(p.bands);}return true;
}
bool matrix_shapes(LoudnessPlan& p) noexcept {
    if(p.kind==LoudnessKind::Salience){p.outputs=2;p.shape[0]=matrix(p.columns,p.rows,true);p.shape[1]=vector(p.rows,true);return true;}
    if(p.kind==LoudnessKind::Meter){p.outputs=5;p.shape[0]=scalar();p.shape[1]=p.shape[2]=vector(p.columns,true);p.shape[3]=vector(p.rows,true,DType::UInt64);p.shape[4]=vector(p.rows,true);return p.columns>=3;}
    if(p.kind==LoudnessKind::Transform){
        if(!p.transform.hop || p.transform.frame_size<2)return false;p.frames=1+(p.rows-1)/p.transform.hop;p.bins=p.transform.frame_size/2+1;
        p.outputs=4;p.shape[0]=matrix(p.frames,p.bins,true);p.shape[1]=vector(p.bins,false,DType::Float64);p.shape[2]=vector(p.frames,true,DType::Float64);p.shape[3]=matrix(p.bins,2,true);return true;
    }
    if(!PreparedActivityFluctuation::geometry(p.rows,p.activity,p.frame_size,p.scales))return false;
    p.outputs=7;p.shape[0]=scalar();p.shape[1]=scalar(DType::UInt8);p.shape[2]=scalar(DType::UInt64);p.shape[3]=vector(p.scales,false,DType::UInt64);
    p.shape[4]=vector(p.scales);p.shape[5]=vector(p.scales-1,true);p.shape[6]=vector(p.rows/p.frame_size,true);return true;
}
bool shapes(const domain::ResolvedProcessingNode& n,LoudnessPlan& p) noexcept {
    if(p.kind<=LoudnessKind::LoopTempo){loop_shapes(p);return p.kind!=LoudnessKind::LoopTempo || p.rate==44100;}
    return p.kind<=LoudnessKind::Sequence?energy_shapes(n,p):matrix_shapes(p);
}
}
bool loudness_operation_kind(std::string_view id) noexcept {for(const auto& e:entries)if(e.id==id)return true;return false;}
std::size_t loudness_operation_inputs(LoudnessKind kind) noexcept {return kind==LoudnessKind::LoopFit || kind==LoudnessKind::Sequence || kind==LoudnessKind::Salience?2:1;}
std::span<const float> loudness_borders(const domain::ResolvedProcessingNode& n,LoudnessKind kind) noexcept {
    if(parameter(n,"borders_hz"))return array_parameter(n,"borders_hz");
    return kind==LoudnessKind::Sequence?std::span<const float>(default_beats_loudness_borders):std::span<const float>(default_single_beat_borders);
}
bool loudness_operation_plan(const domain::ResolvedProcessingNode& n,LoudnessPlan& out) noexcept {
    LoudnessPlan p;bool found=false;for(const auto& e:entries)if(e.id==n.kind){p.kind=e.kind;found=true;break;}
    if(!found || !settings(n,p) || n.inputs.size()!=loudness_operation_inputs(p.kind))return false;
    const bool matrix=p.kind==LoudnessKind::Salience || p.kind==LoudnessKind::Meter || p.kind==LoudnessKind::Transform;
    if(!dimensions(n,0,matrix?Rank::Matrix:Rank::Vector,p.rows,p.columns) || !pcm_clock(n,p))return false;
    std::size_t a=0,b=0;
    if(p.kind==LoudnessKind::LoopFit && !dimensions(n,1,Rank::Scalar,a,b))return false;
    if(p.kind==LoudnessKind::Sequence && !dimensions(n,1,Rank::Vector,p.secondary,b))return false;
    if(p.kind==LoudnessKind::Salience && (!dimensions(n,1,Rank::Vector,a,b) || a!=p.rows))return false;
    if(!shapes(n,p))return false;out=p;return true;
}
}
