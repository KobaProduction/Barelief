// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogramDescriptors at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/beat_interval_histogram.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rhythm_reference_ports {
namespace {
void fill_histogram(std::span<const float> intervals,std::array<float,250>& output) noexcept {
    std::array<std::uint64_t,250> counts{};
    std::uint64_t observations=0;
    for(const float interval:intervals){
        if(interval<60.f/250.f)continue;
        const auto bin=std::min(std::size_t(249),std::size_t(std::round(60.f/interval)));
        ++counts[bin];++observations;
    }
    if(observations)
        for(std::size_t i=0;i<counts.size();++i)output[i]=float(counts[i])/float(observations);
}
TempoHistogramPeak extract_peak(std::array<float,250>& histogram) noexcept {
    const auto bin=std::size_t(std::max_element(histogram.begin(),histogram.end())-histogram.begin());
    TempoHistogramPeak peak;peak.bpm=float(bin);peak.weight=histogram[bin];
    if(bin)peak.weight+=histogram[bin-1];
    if(bin+1<histogram.size())peak.weight+=histogram[bin+1];
    float spread=0;
    const auto first=bin>4?bin-4:0,last=std::min(bin+4,histogram.size()-1);
    for(auto i=first;i<=last;++i){spread+=histogram[i];histogram[i]=0;}
    peak.spread=spread>0?1.f-peak.weight/spread:0;return peak;
}
}
bool beat_interval_histogram(std::span<const float> intervals,BeatIntervalHistogram& output) noexcept {
    if(!std::all_of(intervals.begin(),intervals.end(),[](float x){return std::isfinite(x);}))return false;
    BeatIntervalHistogram next;
    fill_histogram(intervals,next.histogram);
    auto working=next.histogram;
    for(auto& peak:next.peaks)peak=extract_peak(working);
    output=next;return true;
}
}
