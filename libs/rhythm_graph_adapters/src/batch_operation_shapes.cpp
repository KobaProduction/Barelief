// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
BatchShape scalar(DType type=DType::Float32) noexcept {return {Rank::Scalar,1,1,type};}
BatchShape vector(std::size_t size,bool variable=false,ValueDomain values=ValueDomain::FiniteOnly) noexcept {return {Rank::Vector,std::max(std::size_t{1},size),1,DType::Float32,variable,values};}
BatchShape peaks(std::size_t size) noexcept {return {Rank::Matrix,std::max(std::size_t{1},size),2,DType::Float32,true};}
void summary(BatchPlan& p) noexcept {p.shape[0]=p.shape[1]=p.shape[2]=scalar();p.shape[3]=vector(3);}
void pulse_tempo_shape(BatchPlan& p) noexcept {
    summary(p);p.outputs=13;p.shape[4]=vector(414);
    p.shape[5]=p.shape[6]=vector(p.pulse.frame_size/2+1,true);
    p.shape[7]=p.shape[8]=vector(p.pulse.onset_window,true);
    p.shape[9]=p.shape[10]=p.shape[11]=scalar(DType::UInt64);p.shape[12]=peaks(p.pulse.frame_size/2+1);
}
void pulse_correlation_shape(BatchPlan& p) noexcept {
    p.outputs=7;p.shape[0]=scalar();p.shape[1]=p.shape[2]=vector(p.rows);p.shape[3]=peaks(10);
    p.shape[4]=vector(10,true,ValueDomain::AnyIeee);p.shape[5]=p.shape[6]=vector(10,true);
}
void pulse_onset_shape(BatchPlan& p) noexcept {p.outputs=3;p.shape[0]=p.shape[1]=scalar();p.shape[2]=vector(p.rows);}
void pulse_train_shape(BatchPlan& p) noexcept {p.outputs=4;p.shape[0]=scalar();p.shape[1]=p.shape[2]=vector(p.secondary,true);p.shape[3]=vector(p.secondary,true,ValueDomain::AnyIeee);}
void consensus_shape(BatchPlan& p) noexcept {summary(p);p.outputs=7;p.shape[4]=vector(414);p.shape[5]=p.shape[6]=scalar(DType::UInt64);}
bool pulse_shapes(BatchPlan& p) noexcept {
    switch(p.kind){
    case BatchKind::PulseTempo:pulse_tempo_shape(p);return true;
    case BatchKind::PulseOnset:pulse_onset_shape(p);return true;
    case BatchKind::PulseCorrelation:pulse_correlation_shape(p);return true;
    case BatchKind::PulseTrain:pulse_train_shape(p);return true;
    case BatchKind::LagConsensus:consensus_shape(p);return true;
    default:return false;}
}
void fixed_shape(BatchPlan& p) noexcept {
    p.outputs=9;p.shape[0]=peaks(104);p.shape[1]=p.shape[2]=vector(p.rows,true);p.shape[3]=scalar(DType::UInt8);
    p.shape[4]=scalar();p.shape[5]=scalar(DType::UInt64);p.shape[6]=peaks(p.rows);p.shape[7]=p.shape[8]=vector(104,true);
}
void band_shape(BatchPlan& p) noexcept {
    p.outputs=3;p.shape[0]=vector(p.rows-1,true);
    p.shape[1]={Rank::Matrix,std::max(std::size_t{1},p.rows-1),p.columns,DType::Float32,true};
    p.shape[2]={Rank::Matrix,p.band.weighting==NoveltyWeight::Hybrid?4u:1u,p.columns};
}
bool novelty_shapes(BatchPlan& p,std::size_t bins) noexcept {
    switch(p.kind){
    case BatchKind::BandNovelty:band_shape(p);return true;
    case BatchKind::FixedCandidates:fixed_shape(p);return true;
    case BatchKind::FourierCandidates:
        if(p.histogram.maximum_peaks==SIZE_MAX)return false;
        p.outputs=3;p.shape[0]=peaks(p.histogram.maximum_peaks);p.shape[1]=peaks(p.histogram.maximum_peaks+1);p.shape[2]=vector(bins);return true;
    case BatchKind::WeightedHistogram:p.outputs=3;p.shape[0]=vector(bins);p.shape[1]=peaks(bins);p.shape[2]=scalar(DType::UInt8);return true;
    default:return false;}
}
bool phase_shapes(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    std::size_t samples=0;if(!dsp_kernels::prepared_storage::mul_bytes(p.rows,p.histogram.path.hop,samples))return false;
    p.outputs=4;p.shape[0]=vector(samples,true);p.shape[1]=peaks(samples);p.shape[2]=vector(p.columns,true);
    p.shape[3]=vector(array_parameter(n,"synthesis_window").size(),true);return true;
}
void path_shape(BatchPlan& p) noexcept {p.outputs=3;p.shape[0]=vector(p.rows,true);p.shape[1]=scalar();p.shape[2]=scalar(DType::UInt8);}
bool histogram_shapes(BatchPlan& p,std::size_t frames,std::size_t bins,std::size_t hop) noexcept {
    std::size_t samples=0,observations=0;
    if(!dsp_kernels::prepared_storage::mul_bytes(frames,hop,samples) || !dsp_kernels::prepared_storage::mul_bytes(frames,p.histogram.maximum_peaks,observations))return false;
    p.outputs=11;p.shape[0]=scalar();p.shape[1]=peaks(bins);p.shape[2]=vector(frames,true);p.shape[3]={Rank::Matrix,frames,bins,DType::Float32,true};
    p.shape[4]=vector(samples,true);p.shape[5]=peaks(samples);p.shape[6]=scalar(DType::UInt8);p.shape[7]=p.shape[8]=peaks(observations);
    p.shape[9]={Rank::Vector,frames,1,DType::UInt64,true};p.shape[10]=vector(bins);return true;
}
}
bool batch_operation_shapes(const domain::ResolvedProcessingNode& n,BatchPlan& p) noexcept {
    if(pulse_shapes(p))return true;
    const float maximum=std::ceil(p.histogram.path.maximum_bpm);
    if(!std::isfinite(maximum) || maximum<0 || double(maximum)>=double(SIZE_MAX/sizeof(float))-2)return false;
    const auto bins=std::size_t(maximum)+1;
    if(novelty_shapes(p,bins))return true;
    if(p.kind==BatchKind::FramePath){path_shape(p);return true;}
    if(p.kind==BatchKind::PhasePulse)return phase_shapes(n,p);
    if(p.kind==BatchKind::FourierHistogram)return histogram_shapes(p,p.rows,bins,p.histogram.path.hop);
    if(p.kind!=BatchKind::NoveltyHistogram)return false;
    std::size_t length=0,fft=0,hop=0,frames=0;
    if(!PreparedNoveltyTempoHistogram::dimensions(p.rows,p.novelty,length,fft,hop,frames) || !histogram_shapes(p,frames,bins,hop))return false;
    p.outputs=13;p.shape[11]=p.shape[12]=vector(length);p.frames=frames;p.bins=fft/2+1;return true;
}
}
