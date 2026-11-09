// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogram::computeHistogram at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/weighted_tempo_histogram.h>
#include <rhythm_reference_ports/harmonic_tempo.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool valid(const WeightedTempoHistogramConfig& config) noexcept {
    if(!std::isfinite(config.minimum_bpm) || !std::isfinite(config.maximum_bpm) || config.minimum_bpm<0 || config.minimum_bpm>=config.maximum_bpm)return false;
    if(!std::isfinite(config.tolerance_percent) || config.tolerance_percent<0)return false;
    return double(config.maximum_bpm)<double(std::numeric_limits<std::size_t>::max()/sizeof(InterpolatedPeak))-2;
}
bool strength_order(const InterpolatedPeak& a,const InterpolatedPeak& b) noexcept {
    return a.value==b.value?a.position>b.position:a.value>b.value;
}
}
void PreparedWeightedTempoHistogram::allocate(std::size_t observations,std::size_t frames,WeightedTempoHistogramConfig config) {
    config.minimum_bpm=std::floor(config.minimum_bpm);config.maximum_bpm=std::ceil(config.maximum_bpm);
    config_=config;maximum_observations_=observations;maximum_frames_=frames;
    const auto bins=std::size_t(config.maximum_bpm)+1;
    histogram_.resize(bins);frame_.resize(bins);result_.resize(bins);
}
bool PreparedWeightedTempoHistogram::prepare(std::size_t observations,std::size_t frames,WeightedTempoHistogramConfig config) {
    if(!observations || !frames || !valid(config))return false;
    std::size_t bound=0;if(!storage_bytes(observations,frames,config,bound))return false;
    PreparedWeightedTempoHistogram next;
    next.allocate(observations,frames,config);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedWeightedTempoHistogram::read(dsp_kernels::FloatMatrixSource source,std::size_t index,InterpolatedPeak& peak) const noexcept {
    std::array<float,2> row{};
    if(!source.read(source.context,index,row))return false;
    if(!std::isfinite(row[0]) || !std::isfinite(row[1]) || row[0]<0 || row[0]>config_.maximum_bpm || row[1]<0 || std::trunc(row[0])!=row[0])return false;
    peak={row[0],row[1]};return true;
}
bool PreparedWeightedTempoHistogram::scan(dsp_kernels::FloatMatrixSource source,std::size_t count,float& maximum) noexcept {
    maximum=0;silent_=true;
    for(std::size_t i=0;i<count;++i){
        InterpolatedPeak peak;
        if(!read(source,i,peak))return false;
        maximum=std::max(maximum,peak.value);
        if(peak.position>0)silent_=false;
    }
    return true;
}
float PreparedWeightedTempoHistogram::deviation_weight(float candidate,float center) const noexcept {
    if(candidate<config_.minimum_bpm)return 0;
    const float sigma=std::abs(center-candidate)/10.f;
    if(sigma==0)return 1;
    const float k=1.f/(candidate*sigma*std::sqrt(2.f*std::numbers::pi_v<float>));
    const float deviation=std::log(center/candidate);
    return std::exp(-(deviation*deviation)/(k*k));
}
bool PreparedWeightedTempoHistogram::accumulate_at(dsp_kernels::FloatMatrixSource source,std::size_t count,float maximum,const InterpolatedPeak& center) noexcept {
    if(center.position==0)return true;
    auto& value=histogram_[std::size_t(center.position)];
    for(std::size_t j=0;j<count;++j){
        InterpolatedPeak candidate;
        if(!read(source,j,candidate))return false;
        if(candidate.position==0 || !approximately_equal_tempo(center.position,candidate.position,config_.tolerance_percent))continue;
        const float weight=deviation_weight(candidate.position,center.position);
        const float magnitude=maximum>0?candidate.value/maximum:candidate.value;
        value+=config_.weight_by_magnitude?weight*magnitude:weight;
    }
    return std::isfinite(value);
}
bool PreparedWeightedTempoHistogram::accumulate(dsp_kernels::FloatMatrixSource source,std::size_t count,float maximum) noexcept {
    std::fill(histogram_.begin(),histogram_.end(),0);
    for(std::size_t i=0;i<count;++i){
        InterpolatedPeak peak;
        if(!read(source,i,peak) || !accumulate_at(source,count,maximum,peak))return false;
    }
    return true;
}
float PreparedWeightedTempoHistogram::seed_threshold() const noexcept {
    float sum=0;
    std::size_t nonzero=0;
    for(const float value:histogram_)if(value>0){sum+=value;++nonzero;}
    return nonzero?sum/float(nonzero):0;
}
bool PreparedWeightedTempoHistogram::finish_histogram(dsp_kernels::FloatMatrixSource tempogram,std::size_t frames) noexcept {
    seed();
    if(!enrich(tempogram,frames))return false;
    normalize();
    return true;
}
void PreparedWeightedTempoHistogram::seed() noexcept {
    const float threshold=seed_threshold();
    count_=0;
    if(threshold==0){result_[count_++]={0,0};return;}
    for(std::size_t i=0;i<histogram_.size();++i)
        if(histogram_[i]>=threshold)result_[count_++]={float(i),histogram_[i]};
    std::sort(result_.begin(),result_.begin()+std::ptrdiff_t(count_),strength_order);
}
void PreparedWeightedTempoHistogram::add_frame_energy(float total) noexcept {
    for(std::size_t i=0;i<count_;++i){
        const auto first=std::size_t(std::max(0.f,result_[i].position-config_.tolerance_percent));
        const auto last=std::size_t(std::min(float(frame_.size()-1),result_[i].position+config_.tolerance_percent));
        const auto begin=frame_.begin()+std::ptrdiff_t(first),end=frame_.begin()+std::ptrdiff_t(last+1);
        result_[i].value+=std::inner_product(begin,end,begin,0.f)/total;
    }
}
bool PreparedWeightedTempoHistogram::enrich(dsp_kernels::FloatMatrixSource source,std::size_t frames) noexcept {
    for(std::size_t i=0;i<frames;++i){
        if(!source.read(source.context,i,frame_) || !std::all_of(frame_.begin(),frame_.end(),[](float x){return std::isfinite(x);}))return false;
        const float total=std::inner_product(frame_.begin(),frame_.end(),frame_.begin(),0.f);
        if(!std::isfinite(total))return false;
        if(total>0)add_frame_energy(total);
    }
    return true;
}
void PreparedWeightedTempoHistogram::normalize() noexcept {
    std::sort(result_.begin(),result_.begin()+std::ptrdiff_t(count_),strength_order);
    if(!count_)return;
    const float maximum=result_[0].value;
    std::size_t accepted=0;
    for(std::size_t i=0;i<count_;++i){
        const float value=maximum>0?result_[i].value/maximum:result_[i].value;
        if(value>=.25f)result_[accepted++]={result_[i].position,value};
    }
    count_=accepted;
}
bool PreparedWeightedTempoHistogram::process(dsp_kernels::FloatMatrixSource observations,std::size_t count,
                                             dsp_kernels::FloatMatrixSource tempogram,std::size_t frames) noexcept {
    if(histogram_.empty() || count>maximum_observations_ || frames>maximum_frames_)return false;
    if((count && !observations.read) || (frames && !tempogram.read))return false;
    float maximum=0;
    if(!scan(observations,count,maximum))return false;
    count_=0;std::fill(histogram_.begin(),histogram_.end(),0);
    if(silent_)return true;
    if(!accumulate(observations,count,maximum))return false;
    return finish_histogram(tempogram,frames);
}
}
