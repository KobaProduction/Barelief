// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_operations.h"
#include <algorithm>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
struct Named {std::string_view name;EssentiaKind kind;};
constexpr Named names[]{{"EssentiaOnsetRows",EssentiaKind::Onset},
    {"EssentiaInformationRows",EssentiaKind::Information},{"EssentiaNoveltyRows",EssentiaKind::Novelty},
    {"EssentiaErbRows",EssentiaKind::Erb},{"EssentiaAgreement",EssentiaKind::Agreement},
    {"EssentiaCandidateInputs",EssentiaKind::CandidateInputs}};
BatchShape vector(std::size_t n) noexcept {return {Rank::Vector,n,1,DType::Float32,true};}
BatchShape matrix(std::size_t r,std::size_t c) noexcept {return {Rank::Matrix,r,c,DType::Float32,true};}
bool native(const NumericContract& c,Rank rank) noexcept {
    return c.dtype==DType::Float32 && c.shape.rank==rank && c.channel_order.size()==1 &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        !c.representation.valid_bits && c.representation.normalization==Normalization::None;
}
bool same_spectral_frame(const NumericContract& first,const NumericContract& next) noexcept {
    return next.shape==first.shape && next.coordinates==first.coordinates && equal_frame_clock(next.clock,first.clock);
}
bool vector_inputs(EssentiaKind kind) noexcept {
    return kind==EssentiaKind::Agreement || kind==EssentiaKind::CandidateInputs;
}
bool extra_inputs(const domain::ResolvedProcessingNode& n,EssentiaPlan& p,const NumericContract& first) noexcept {
    for(std::size_t i=1;i<p.inputs;++i){
        NumericContract next;if(n.inputs[i].id!=i || !numeric_port(n,true,domain::PortId(i),next) || !native(next,first.shape.rank))return false;
        p.counts[i]=std::size_t(next.shape.extents[0].maximum);
        if(vector_inputs(p.kind))p.rows=std::max(p.rows,p.counts[i]);
        else if(!same_spectral_frame(first,next))return false;
    }
    return p.rows<=65536;
}
bool spectral_geometry(const NumericContract& c,EssentiaKind kind) noexcept {
    return vector_inputs(kind) || (essentia_frequency_contract(c) && c.shape.extents[1]==ExtentBounds{1025,1025});
}
bool inputs(const domain::ResolvedProcessingNode& n,EssentiaPlan& p) noexcept {
    if(n.inputs.size()!=p.inputs)return false;
    NumericContract first;
    if(!numeric_port(n,true,0,first) || !native(first,vector_inputs(p.kind)?Rank::Vector:Rank::Matrix) ||
       !spectral_geometry(first,p.kind))return false;
    p.rows=std::size_t(first.shape.extents[0].maximum);
    p.counts[0]=p.rows;
    return p.rows && p.rows<=65536 && n.inputs[0].id==0 && extra_inputs(n,p,first);
}
void onset_shapes(EssentiaPlan& p) noexcept {
    if(p.kind==EssentiaKind::Novelty)p.shapes[0]=matrix(p.rows,p.bins);
    if(p.kind==EssentiaKind::Onset || p.kind==EssentiaKind::Information){
        p.outputs=2;p.shapes[1]=vector(p.rows);p.shapes[1].type=DType::Float64;
    }
    if(p.kind==EssentiaKind::Onset && p.method!=5){p.outputs=3;p.shapes[2]=matrix(p.rows,p.method==4?40:p.bins);}
}
void selection_shapes(EssentiaPlan& p) noexcept {
    if(p.kind==EssentiaKind::Erb){
        p.outputs=3;p.shapes[0]=matrix(p.rows,40);p.shapes[1]={Rank::Vector,40};p.shapes[2]={Rank::Matrix,40,p.bins};
    }
    if(p.kind==EssentiaKind::Agreement){
        p.outputs=5;p.shapes[1]={Rank::Scalar,1};p.shapes[2]={Rank::Scalar,1,1,DType::UInt64};
        p.shapes[3]={Rank::Vector,5};p.shapes[4]={Rank::Matrix,5,5};
    }
}
void shapes(EssentiaPlan& p) noexcept {
    p.shapes[0]=vector(p.rows);onset_shapes(p);selection_shapes(p);
    if(p.kind==EssentiaKind::CandidateInputs){
        p.outputs=5;for(std::size_t i=0;i<5;++i)p.shapes[i]=vector(p.counts[i]);
    }
}
bool identify(std::string_view kind,EssentiaKind& result) noexcept {
    for(const auto& item:names)if(item.name==kind){result=item.kind;return true;}
    return false;
}
bool settings(const domain::ResolvedProcessingNode& n,EssentiaPlan& p) noexcept {
    if(!integer_parameter(n,"method",1,p.method) || !scalar_parameter(n,"minimum_time",5,p.minimum_time) ||
       !scalar_parameter(n,"frame_rate",p.kind==EssentiaKind::Onset?44100.f/1024.f:44100.f/512.f,p.frame_rate) ||
       p.frame_rate<=0)return false;
    if(p.kind==EssentiaKind::Onset && p.method!=1 && p.method!=4 && p.method!=5)return false;
    p.inputs=vector_inputs(p.kind)?5:(p.kind==EssentiaKind::Novelty || (p.kind==EssentiaKind::Onset && p.method==1))?2:1;
    return true;
}
}
bool essentia_operation_kind(std::string_view name) noexcept {
    for(const auto& item:names)if(item.name==name)return true;
    return false;
}
bool essentia_operation_plan(const domain::ResolvedProcessingNode& n,EssentiaPlan& p) noexcept {
    p={};
    if(!identify(n.kind,p.kind) || !settings(n,p) || !inputs(n,p))return false;
    shapes(p);return true;
}
}
