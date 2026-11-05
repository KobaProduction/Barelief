// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia SingleBeatLoudness/EnergyBandRatio at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/spectral_band_ratios.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool valid(std::size_t bins,float rate,std::span<const float> borders) noexcept {
    if(!bins || bins>std::size_t(std::numeric_limits<int>::max()) || !std::isfinite(rate) || rate<=0 ||
       rate/2.f<=0 || borders.empty() || borders.size()-1>SIZE_MAX/bins)return false;
    float previous=0;
    for(const float border:borders){
        if(!std::isfinite(border) || border<previous)return false;
        previous=border;
    }
    return true;
}
bool support(std::size_t bins,float nyquist,float low,float high,dsp_kernels::FilterBandSupport& result,bool inclusive) noexcept {
    const float step=bins>1?nyquist/float(bins-1):nyquist;
    const float first=(inclusive?low/nyquist*float(bins-1):low/step)+.5f;
    const float last=(inclusive?high/nyquist*float(bins-1):high/step)+.5f;
    if(!std::isfinite(first) || !std::isfinite(last))return false;
    const auto begin=first>=float(bins)?bins:std::size_t(first);
    const auto end=last>=float(bins)?bins:std::min(bins,std::size_t(last)+std::size_t(inclusive));
    result={begin,end>begin?end-begin:0};return true;
}
bool prepare_projection(dsp_kernels::MelFilterBank& projection,std::size_t bins,float rate,std::span<const float> borders,bool inclusive=true) {
    const auto bands=borders.size()-1;
    std::vector<float> weights(bands*bins,0.f);std::vector<double> centers(bands);
    std::vector<dsp_kernels::FilterBandSupport> supports(bands);
    for(std::size_t i=0;i<bands;++i){
        if(!support(bins,rate/2.f,borders[i],borders[i+1],supports[i],inclusive))return false;
        std::fill_n(weights.begin()+std::ptrdiff_t(i*bins+supports[i].first),supports[i].count,1.f);
        centers[i]=double(borders[i])*.5+double(borders[i+1])*.5;
    }
    return projection.prepare_matrix(bins,bands,weights,centers,{false,true},supports);
}
}
bool PreparedFrequencyBandEnergy::prepare(std::size_t bins,float rate,std::span<const float> borders) {
    if(bins<2 || borders.size()<2 || !valid(bins,rate,borders) ||
       std::adjacent_find(borders.begin(),borders.end())!=borders.end())return false;
    std::size_t bound=0;if(!storage_bytes(bins,rate,borders,bound))return false;
    dsp_kernels::MelFilterBank next;
    if(!prepare_projection(next,bins,rate,borders,false) || next.workspace_bytes()>bound)return false;
    projection_=std::move(next);return true;
}
bool PreparedSpectralBandRatios::configure(std::size_t bins,float rate,std::span<const float> borders) {
    if(!valid(bins,rate,borders))return false;
    dsp_kernels::SpectralDescriptorConfig energy;energy.method=dsp_kernels::SpectralDescriptor::SpectralEnergy;
    energy.aggregation=dsp_kernels::SpectralAggregation::Sum;energy.compensated_sum=false;
    if(!energy_.prepare(bins,energy))return false;
    if(borders.size()>1 && !prepare_projection(projection_,bins,rate,borders))return false;
    result_.resize(borders.size()-1);ready_=true;return true;
}
bool PreparedSpectralBandRatios::prepare(std::size_t bins,float rate,std::span<const float> borders) {
    std::size_t bound=0;if(!storage_bytes(bins,rate,borders,bound))return false;
    PreparedSpectralBandRatios next;
    if(!next.configure(bins,rate,borders) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedSpectralBandRatios::process(std::span<const float> magnitude,float& energy,std::span<float> ratios) noexcept {
    if(!ready_ || ratios.size()!=result_.size())return false;
    float total=0;
    if(!energy_.process_values(magnitude,total))return false;
    std::fill(result_.begin(),result_.end(),0.f);
    if(total>1e-10f && !result_.empty()){
        if(!projection_.process(magnitude,result_))return false;
        for(auto& value:result_)value/=total;
    }
    std::copy(result_.begin(),result_.end(),ratios.begin());energy=total;return true;
}
}
