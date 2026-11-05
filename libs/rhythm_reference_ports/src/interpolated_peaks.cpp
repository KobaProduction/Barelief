// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of MTG/essentia PeakDetection at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <dsp_kernels/peaks.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool amplitude_order(const InterpolatedPeak& a,const InterpolatedPeak& b) noexcept {
    return a.value==b.value?a.position<b.position:a.value>b.value;
}
bool position_order(const InterpolatedPeak& a,const InterpolatedPeak& b) noexcept {
    return a.position==b.position?a.value>b.value:a.position<b.position;
}
}
bool PreparedInterpolatedPeaks::configure(std::size_t capacity,InterpolatedPeakConfig config) {
    config_=config;capacity_=capacity;peaks_.resize(capacity);return true;
}
bool PreparedInterpolatedPeaks::storage_bytes(std::size_t capacity,InterpolatedPeakConfig config,std::size_t& bytes) noexcept {
    if(capacity<2 || !config.maximum_peaks || !std::isfinite(config.range) || config.range<=0)return false;
    if(!std::isfinite(config.minimum) || !std::isfinite(config.maximum) || config.minimum<0 || config.minimum>=config.maximum)return false;
    if(!std::isfinite(config.threshold) || !std::isfinite(config.minimum_distance) || config.minimum_distance<0)return false;
    if(config.ordering!=PeakOrdering::Position && config.ordering!=PeakOrdering::Amplitude)return false;
    return dsp_kernels::prepared_storage::mul_bytes(capacity,sizeof(InterpolatedPeak),bytes);
}
bool PreparedInterpolatedPeaks::prepare(std::size_t capacity,InterpolatedPeakConfig config) {
    PreparedInterpolatedPeaks next;std::size_t bytes=0;
    if(!storage_bytes(capacity,config,bytes) || !next.configure(capacity,config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedInterpolatedPeaks::append(InterpolatedPeak peak) noexcept {
    if(count_<peaks_.size())peaks_[count_++]=peak;
}
InterpolatedPeak PreparedInterpolatedPeaks::candidate(std::span<const float> input,std::size_t first,std::size_t last,float scale) const noexcept {
    float bin=float(first),value=input[first];
    if(config_.interpolate && first!=last)bin=float(first+last)*.5f;
    if(config_.interpolate && first==last && first && first+1<input.size()){
        const float shift=dsp_kernels::quadratic_peak_offset(input[first-1],input[first],input[first+1]);
        bin+=shift;value-=.25f*(input[first-1]-input[first+1])*shift;
    }
    return {bin*scale,value};
}
bool PreparedInterpolatedPeaks::walk(std::span<const float> input,std::size_t& i,float scale) noexcept {
    const auto last=input.size()-1;
    while(i+1<last && input[i]>=input[i+1])++i;
    while(i+1<last && input[i]<input[i+1])++i;
    auto j=i;
    while(j+1<last && input[j]==input[j+1])++j;
    if(j+1<last && input[j+1]<input[j] && input[j]>config_.threshold){
        const auto peak=candidate(input,i,j,scale);
        if(peak.position>config_.maximum)return false;
        append(peak);
    }
    i=j;return i+1<last;
}
void PreparedInterpolatedPeaks::collect(std::span<const float> input,float scale) noexcept {
    const float start=std::ceil(config_.minimum/scale);
    if(start>=float(input.size()))return;
    auto i=std::size_t(start);
    if(i+1<input.size() && input[i]>input[i+1] && input[i]>config_.threshold)append({float(i)*scale,input[i]});
    while(walk(input,i,scale)){}
    if(i==input.size()-2 && i && input[i-1]<input[i] && input[i+1]<input[i] && input[i]>config_.threshold)
        append(candidate(input,i,i,scale));
    const auto last=input.size()-1;
    const float upper=config_.maximum/scale;
    if(float(last-1)<upper && upper<=float(last) && input[last]>input[last-1] && input[last]>config_.threshold)
        append({float(last)*scale,input[last]});
}
bool PreparedInterpolatedPeaks::separated(std::size_t index,std::size_t accepted) const noexcept {
    for(std::size_t j=0;j<accepted;++j)
        if(std::abs(peaks_[index].position-peaks_[j].position)<config_.minimum_distance)return false;
    return true;
}
void PreparedInterpolatedPeaks::distance_filter() noexcept {
    std::sort(peaks_.begin(),peaks_.begin()+std::ptrdiff_t(count_),amplitude_order);
    std::size_t accepted=0;
    for(std::size_t i=0;i<count_;++i)if(separated(i,accepted))peaks_[accepted++]=peaks_[i];
    count_=accepted;
}
void PreparedInterpolatedPeaks::order() noexcept {
    if(config_.minimum_distance>0 && count_>1)distance_filter();
    if(config_.ordering==PeakOrdering::Amplitude)
        std::sort(peaks_.begin(),peaks_.begin()+std::ptrdiff_t(count_),amplitude_order);
    else std::sort(peaks_.begin(),peaks_.begin()+std::ptrdiff_t(count_),position_order);
    count_=std::min(count_,config_.maximum_peaks);
}
bool PreparedInterpolatedPeaks::set_threshold(float threshold) noexcept {
    if(!capacity_ || !std::isfinite(threshold))return false;
    config_.threshold=threshold;return true;
}
bool PreparedInterpolatedPeaks::set_coordinates(float range,float minimum,float maximum) noexcept {
    if(!capacity_ || !std::isfinite(range) || range<=0 || !std::isfinite(minimum) ||
       !std::isfinite(maximum) || minimum<0 || minimum>=maximum)return false;
    config_.range=range;config_.minimum=minimum;config_.maximum=maximum;return true;
}
bool PreparedInterpolatedPeaks::process(std::span<const float> input) noexcept {
    if(input.size()<2 || input.size()>capacity_)return false;
    if(!std::all_of(input.begin(),input.end(),[](float value){return std::isfinite(value);}))return false;
    count_=0;collect(input,config_.range/float(input.size()-1));order();return true;
}
}
