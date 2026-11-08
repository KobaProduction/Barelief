// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
#include "frame_operation_io.h"
#include <cmath>
#include <climits>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
struct Entry {std::string_view id;LoudnessKind kind;};
constexpr Entry entries[]{{"LoopLengthFit",LoudnessKind::LoopFit},{"SpectralBandRatios",LoudnessKind::Ratios},
    {"FrequencyBandEnergy",LoudnessKind::Frequency},{"BeatBandSalience",LoudnessKind::Salience}};
bool dimensions(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId id,Rank rank,std::size_t& a,std::size_t& b) noexcept {
    NumericBufferContract c;if(!numeric_port(n,true,id,c) || c.dtype!=DType::Float32 || c.shape.rank!=rank || c.plane_count!=1 ||
        c.representation.encoding!=StorageEncoding::Native || c.representation.layout!=ChannelLayout::Planar || c.representation.valid_bits ||
        (c.representation.normalization!=Normalization::None && c.representation.normalization!=Normalization::FloatUnit))return false;
    if(c.shape.extents[0].maximum>INT_MAX || c.shape.extents[1].maximum>INT_MAX)return false;
    a=std::size_t(c.shape.extents[0].maximum);b=std::size_t(c.shape.extents[1].maximum);return a && b;
}

bool settings(const dataflow_runtime::NumericOperationNodeView& n,LoudnessPlan& p) noexcept {
    return scalar_parameter(n,"sample_rate",44100,p.rate) && std::isfinite(p.rate) && p.rate>0 && integer_parameter(n,"window_size",16,p.window);
}
BatchShape scalar(DType type=DType::Float32) noexcept {return {Rank::Scalar,1,1,type};}
BatchShape vector(std::size_t n,bool variable=false,DType type=DType::Float32) noexcept {return {Rank::Vector,std::max(std::size_t{1},n),1,type,variable};}
BatchShape matrix(std::size_t a,std::size_t b,bool variable=false) noexcept {return {Rank::Matrix,std::max(std::size_t{1},a),b,DType::Float32,variable};}
void loop_shapes(LoudnessPlan& p) noexcept {
    p.outputs=6;for(std::size_t i=0;i<p.outputs;++i)p.shape[i]=scalar();
    p.shape[2]=p.shape[3]=scalar(DType::UInt64);p.shape[4]=vector(4,false,DType::Int64);p.shape[5]=vector(4);
}
void band_energy_shape(LoudnessPlan& p) noexcept {
    p.outputs=p.kind==LoudnessKind::Ratios?2:1;p.shape[0]=vector(p.bands);
    if(p.outputs==2){p.shape[0]=scalar();p.shape[1]=vector(p.bands);}
}
bool energy_shapes(const dataflow_runtime::NumericOperationNodeView& n,LoudnessPlan& p) noexcept {
    const auto borders=loudness_borders(n,p.kind);if(borders.size()<2)return false;
    p.bands=borders.size()-1;
    band_energy_shape(p);
    return true;
}
bool salience_shape(LoudnessPlan& p) noexcept {
    p.outputs=2;p.shape[0]=matrix(p.columns,p.rows,true);p.shape[1]=vector(p.rows,true);return true;
}
bool shapes(const dataflow_runtime::NumericOperationNodeView& n,LoudnessPlan& p) noexcept {
    if(p.kind==LoudnessKind::LoopFit){loop_shapes(p);return true;}
    return p.kind==LoudnessKind::Salience?salience_shape(p):energy_shapes(n,p);

}
}
bool loudness_operation_kind(std::string_view id) noexcept {for(const auto& e:entries)if(e.id==id)return true;return false;}
std::size_t loudness_operation_inputs(LoudnessKind kind) noexcept {return kind==LoudnessKind::LoopFit || kind==LoudnessKind::Salience?2:1;}
std::span<const float> loudness_borders(const dataflow_runtime::NumericOperationNodeView& n,LoudnessKind) noexcept {
    if(parameter(n,"borders_hz"))return array_parameter(n,"borders_hz");
    return default_single_beat_borders;
}
namespace {
bool loudness_kind(std::string_view kind,LoudnessPlan& plan) noexcept {
    for(const auto& entry:entries)if(entry.id==kind){plan.kind=entry.kind;return true;}
    return false;
}
bool loudness_input_geometry(const dataflow_runtime::NumericOperationNodeView& n,LoudnessPlan& p) noexcept {
    const bool matrix=p.kind==LoudnessKind::Salience;
    if(!dimensions(n,0,matrix?Rank::Matrix:Rank::Vector,p.rows,p.columns))return false;
    std::size_t a=0,b=0;
    if(p.kind==LoudnessKind::LoopFit)return dimensions(n,1,Rank::Scalar,a,b);
    return p.kind!=LoudnessKind::Salience || (dimensions(n,1,Rank::Vector,a,b) && a==p.rows);
}
}
bool loudness_operation_plan(const dataflow_runtime::NumericOperationNodeView& n,LoudnessPlan& out) noexcept {
    LoudnessPlan p;
    if(!loudness_kind(n.kind,p) || !settings(n,p) || n.inputs.size()!=loudness_operation_inputs(p.kind))return false;
    if(!loudness_input_geometry(n,p) || !shapes(n,p))return false;
    out=p;return true;
}
}
