// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
BatchShape scalar(DType t=DType::Float32) noexcept {return {Rank::Scalar,1,1,t};}
BatchShape vector(std::size_t n,DType t=DType::Float32) noexcept {return {Rank::Vector,std::max(std::size_t{1},n),1,t,true};}
BatchShape matrix(std::size_t n,std::size_t c,DType t=DType::Float32) noexcept {return {Rank::Matrix,std::max(std::size_t{1},n),c,t,true};}
void spectrum(TrackingPlan& p,std::size_t first,std::size_t size) noexcept {
    const auto bins=size/2+1;p.shape[first]=p.shape[first+1]=vector(bins);p.shape[first+2]=matrix(bins,2);
    p.shape[first+3]=p.shape[first+4]=p.shape[first+5]=vector(size);p.shape[first+6]=scalar(DType::Float64);p.shape[first+7]=scalar();
}
bool frame_count(std::size_t samples,std::size_t size,std::size_t hop,bool center,std::size_t& out) noexcept {
    dsp_kernels::TemporalWindowConfig w;w.length=size;w.hop=hop;w.center=center;w.odd_center_ceil=center;w.tail=center?dsp_kernels::TemporalTail::CenterCoverEnd:dsp_kernels::TemporalTail::CoverEnd;
    return dsp_kernels::PreparedTemporalWindow::frame_count(w,samples,out);
}
bool global_shapes(TrackingPlan& p) noexcept {
    if(p.global_recipe){std::size_t frames=0;if(!frame_count(p.rows,p.global.frame_size,p.global.hop,false,frames))return false;
        p.outputs=13;p.shape[0]=vector(frames);p.shape[1]=vector(40);p.shape[2]=vector(p.global.frame_size/2+1);p.shape[3]=p.shape[4]=vector(40);spectrum(p,5,p.global.frame_size);return true;}
    switch(p.kind){
    case TrackingKind::InformationGain:p.outputs=1;p.shape[0]=scalar();return true;
    case TrackingKind::BandEmphasis:p.outputs=2;p.shape[0]=vector(p.rows);p.shape[1]=vector(p.columns);return true;
    case TrackingKind::Erb:p.outputs=3;p.shape[0]=p.shape[1]=vector(p.erb.bands);p.shape[2]=matrix(p.erb.bands,p.rows);return true;
    case TrackingKind::PcmBeat:{
        std::size_t frames=0,more=0;if(!frame_count(p.rows,2048,1024,false,frames) || !frame_count(p.rows,2048,512,false,more) || frames>(SIZE_MAX-1)/2)return false;
        const auto count=std::max(2*frames+1,more);p.outputs=21;p.shape[0]=vector(count);p.shape[1]=scalar();p.shape[2]=vector(5);
        for(std::size_t i=0;i<5;++i){p.shape[3+i]=vector(i<3?frames:more);p.shape[8+i]=vector(count);}spectrum(p,13,2048);return true;}
    default:return false;}
}
bool period_shapes(TrackingPlan& p) noexcept {
    switch(p.kind){
    case TrackingKind::Threshold:p.outputs=1;p.shape[0]=vector(p.rows);return true;
    case TrackingKind::PeriodPath:p.outputs=7;p.shape[0]=p.shape[1]=vector(p.rows);p.shape[2]=matrix(p.rows,p.path.hop);p.shape[3]=vector(p.rows,DType::UInt32);p.shape[4]=vector(p.path.frame_length);p.shape[5]=p.shape[6]=vector(p.path.hop);return true;
    case TrackingKind::Renewal:p.outputs=2;p.shape[0]=vector(p.rows,DType::UInt8);p.shape[1]=vector(p.rows,DType::UInt32);return true;
    case TrackingKind::PeriodBeat:{
        std::size_t n=0;if(!dsp_kernels::endpoint_expansion_size(p.rows,p.beat.upsampling,n))return false;p.outputs=6;
        p.shape[0]=vector(n,DType::UInt8);p.shape[1]=vector(n);p.shape[2]=p.shape[3]=vector(n);p.shape[4]=vector(n);p.shape[5]=vector(n,DType::UInt32);return true;}
    case TrackingKind::Agreement:p.outputs=4;p.shape[0]=scalar(DType::UInt64);p.shape[1]=scalar();p.shape[2]=vector(p.rows);p.shape[3]=matrix(p.rows,p.rows);return true;
    case TrackingKind::MultiBeat:{p.outputs=9;std::size_t maximum=1;
        for(std::size_t i=0;i<5;++i){std::size_t n=0;if(!dsp_kernels::endpoint_expansion_size(p.counts[i],p.multi.upsampling[i],n))return false;p.shape[4+i]=vector(n);maximum=std::max(maximum,n);}
        p.shape[0]=vector(maximum);p.shape[1]=scalar();p.shape[2]=scalar(DType::UInt64);p.shape[3]=vector(5);return true;}
    default:return false;}
}
void legacy_shapes(TrackingPlan& p) noexcept {
    switch(p.kind){
    case TrackingKind::BandFeatures:p.outputs=2;p.shape[0]=vector(p.rows);p.shape[1]=scalar();break;
    case TrackingKind::CombPeriod:p.outputs=6;p.shape[0]=p.shape[1]=scalar();p.shape[2]=vector(p.rows);p.shape[3]=p.shape[4]=vector(p.rows/4);p.shape[5]=vector(std::size_t(std::round(float(p.rows)/4.f)));break;
    case TrackingKind::FeaturePeriod:p.outputs=3;p.shape[0]=p.shape[1]=vector(p.rows);p.shape[2]=scalar(DType::UInt8);break;
    case TrackingKind::PhaseTicks:p.outputs=5;p.shape[0]=vector(p.maximum_ticks);p.shape[1]=vector(p.maximum_matches);p.shape[2]=scalar(DType::Int32);p.shape[3]=scalar(DType::UInt64);p.shape[4]=scalar(DType::UInt8);break;
    case TrackingKind::HarmonicComb:p.outputs=1;p.shape[0]=vector(p.response_length);break;
    case TrackingKind::PhaseResponse:p.outputs=1;p.shape[0]=vector(p.response_length);break;
    case TrackingKind::Silence:p.outputs=4;p.shape[0]=vector(p.rows);p.shape[1]=p.shape[2]=p.shape[3]=scalar(DType::UInt64);break;
    case TrackingKind::LagPool:p.outputs=4;p.shape[0]=p.shape[1]=vector(p.maximum_matches);p.shape[2]=p.shape[3]=scalar(DType::UInt64);break;
    case TrackingKind::TickRefinement:p.outputs=3;p.shape[0]=p.shape[1]=vector(p.maximum_ticks);p.shape[2]=scalar(DType::UInt8);break;
    case TrackingKind::LagVote:p.outputs=2;p.shape[0]=scalar();p.shape[1]=vector(p.rows);break;
    case TrackingKind::Legacy:p.outputs=20;p.shape[0]=p.shape[1]=vector(p.maximum_ticks);p.shape[2]=scalar();p.shape[3]=vector(p.maximum_matches);p.shape[4]=scalar(DType::UInt8);
        p.shape[5]=vector(11);p.shape[6]=vector(8);p.shape[7]=p.shape[8]=vector(11);p.shape[9]=vector(p.maximum_ticks);p.shape[10]=vector(p.maximum_matches);p.shape[11]=vector(3,DType::UInt64);spectrum(p,12,p.legacy.audio_frame);break;
    default:break;}
}
}
bool tracking_operation_shapes(const domain::ResolvedProcessingNode&,TrackingPlan& p) noexcept {
    if(global_shapes(p) || period_shapes(p))return true;legacy_shapes(p);return p.outputs!=0;
}
} // namespace rhythm_graph_adapters::detail
