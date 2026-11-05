// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia NoveltyCurveFixedBpmEstimator histogram/merge operations.
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/tempo_distribution_peaks.h>
#include <rhythm_reference_ports/harmonic_tempo.h>
#include <dsp_kernels/value_histogram.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedTempoDistributionPeaks::storage_bytes(std::size_t& out) noexcept {
    InterpolatedPeakConfig c;c.range=104;c.maximum=104;c.threshold=0;c.maximum_peaks=104;c.ordering=PeakOrdering::Amplitude;
    return PreparedInterpolatedPeaks::storage_bytes(104,c,out);
}
std::size_t PreparedTempoDistributionPeaks::workspace_bytes() const noexcept {return detector_.workspace_bytes();}
bool PreparedTempoDistributionPeaks::prepare() {
    std::size_t bytes=0;if(!storage_bytes(bytes))return false;
    InterpolatedPeakConfig config;
    config.range=104;config.maximum=104;config.threshold=0;config.maximum_peaks=104;config.ordering=PeakOrdering::Amplitude;
    PreparedTempoDistributionPeaks next;
    if(!next.detector_.prepare(104,config) || next.workspace_bytes()>bytes)return false;
    next.prepared_=true;*this=std::move(next);return true;
}
bool PreparedTempoDistributionPeaks::histogram(std::span<const float> values) noexcept {
    std::array<std::uint64_t,100> counts{};
    if(!dsp_kernels::uniform_value_histogram(values,counts,std::span(centers_).first(100)))return false;
    for(std::size_t i=0;i<counts.size();++i)histogram_[i]=float(counts[i]);
    for(std::size_t i=100;i<104;++i){histogram_[i]=0;centers_[i]=centers_[i-1]+centers_[i-1]-centers_[i-2];}
    return true;
}
bool PreparedTempoDistributionPeaks::map_peaks() noexcept {
    const auto detected=detector_.peaks();
    for(std::size_t i=0;i<detected.size();++i){
        const float index=std::round(detected[i].position);
        if(index<0 || index>=float(centers_.size()))return false;
        peaks_[i]={std::round(centers_[std::size_t(index)]),detected[i].value};
    }
    count_=detected.size();return true;
}
bool PreparedTempoDistributionPeaks::process(std::span<const float> values) noexcept {
    if(!prepared_)return false;
    if(values.empty()){count_=0;histogram_.fill(0);centers_.fill(0);return true;}
    if(!histogram(values) || !detector_.process(histogram_))return false;
    return map_peaks();
}
bool PreparedTempoDistributionPeaks::merge_pair(std::size_t left,std::size_t right,float tolerance) noexcept {
    if(!approximately_equal_tempo(peaks_[left].position,peaks_[right].position,tolerance))return false;
    const auto a=peaks_[left],b=peaks_[right];
    peaks_[left]={(a.position*a.value+b.position*b.value)/(a.value+b.value),a.value+b.value};
    std::move(peaks_.begin()+std::ptrdiff_t(right+1),peaks_.begin()+std::ptrdiff_t(count_),peaks_.begin()+std::ptrdiff_t(right));
    --count_;return true;
}
void PreparedTempoDistributionPeaks::sort_by_magnitude() noexcept {
    std::sort(peaks_.begin(),peaks_.begin()+std::ptrdiff_t(count_),[](const auto& a,const auto& b){
        return a.value==b.value?a.position>b.position:a.value>b.value;
    });
}
bool PreparedTempoDistributionPeaks::merge(float tolerance) noexcept {
    if(!prepared_ || !std::isfinite(tolerance) || tolerance<0)return false;
    for(std::size_t left=0;left<count_;++left)
        for(auto right=left+1;right<count_;)if(!merge_pair(left,right,tolerance))++right;
    sort_by_magnitude();return true;
}
}
