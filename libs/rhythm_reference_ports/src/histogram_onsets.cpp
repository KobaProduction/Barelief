// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Aubio src/utils/hist.c, src/utils/scale.c and src/spectral/specdesc.c,
// revision ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2009 Paul Brossier.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version3 or later, without warranty. See ../COPYING.
#include <rhythm_reference_ports/histogram_onsets.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
namespace rhythm_reference_ports {
namespace {
float source_unwrap_phase(float phase) noexcept {
    // Aubio Float32 uses Double PI/TWO_PI constants, floorf's narrowed
    // argument, and one final Float32 conversion (mathutils.c:155).
    constexpr double pi=std::numbers::pi_v<double>,tau=2.*pi;
    const float turns=std::floor(float(-(double(phase)+pi)/tau));
    return float(double(phase)+tau*(1.+double(turns)));
}
}
bool PreparedHistogram::storage_bytes(HistogramConfig c,std::size_t& bytes) noexcept {
    if(!c.bins || c.bins>SIZE_MAX/(2*sizeof(float)) || !std::isfinite(c.minimum) || !std::isfinite(c.maximum) || c.maximum<c.minimum)return false;
    bytes=2*c.bins*sizeof(float);return true;
}
bool PreparedHistogram::prepare(HistogramConfig c) {
    std::size_t bytes=0;if(!storage_bytes(c,bytes))return false;
    PreparedHistogram next;next.config_=c;next.minimum_=c.minimum;next.maximum_=c.maximum;
    next.counts_.resize(c.bins);next.centers_.resize(c.bins);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedHistogram::adapt(std::span<const float> values,bool dynamic) noexcept {
    const auto [minimum,maximum]=std::minmax_element(values.begin(),values.end());
    const float low=dynamic?*minimum:minimum_,high=dynamic?*maximum:maximum_;
    const float range=high-low,scale=range!=0?float(counts_.size())/range:0;
    if(!std::isfinite(range) || !std::isfinite(scale))return false;
    minimum_=low;maximum_=high;step_=range/float(counts_.size());scale_=scale;return true;
}
void PreparedHistogram::accumulate(std::span<float> values,bool ignore_zero) noexcept {
    std::fill(counts_.begin(),counts_.end(),0.f);
    for(auto& value:values){
        value=(value-minimum_)*scale_;
        if(value<0 || value>=float(counts_.size()) || (ignore_zero && value==0))continue;
        ++counts_[std::size_t(std::floor(value))];
    }
}
bool PreparedHistogram::result(bool weighted,float& output) noexcept {
    float sum=0;
    for(std::size_t bin=0;bin<counts_.size();++bin){
        centers_[bin]=minimum_+.5f*step_+float(bin)*step_;
        if(weighted)counts_[bin]*=centers_[bin];
        sum+=counts_[bin];
    }
    sum/=float(counts_.size());if(!std::isfinite(sum))return false;
    output=sum;return true;
}
bool PreparedHistogram::process(std::span<float> values,float& output,bool dynamic,bool ignore_zero,bool weighted) noexcept {
    if(counts_.empty() || values.empty() || !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    if(!adapt(values,dynamic))return false;
    accumulate(values,ignore_zero);return result(weighted,output);
}
bool PreparedHistogramOnset::storage_bytes(std::size_t bins,HistogramOnsetConfig c,std::size_t& bytes) noexcept {
    if(!bins || bins>SIZE_MAX/(6*sizeof(float)) || !std::isfinite(c.magnitude_threshold) || c.magnitude_threshold<0)return false;
    if(c.method<HistogramOnsetMethod::Phase || c.method>HistogramOnsetMethod::SpectralDifference)return false;
    std::size_t histogram=0;if(!PreparedHistogram::storage_bytes(c.histogram,histogram))return false;
    const auto values=6*bins*sizeof(float);if(histogram>SIZE_MAX-values)return false;
    bytes=histogram+values;return true;
}
bool PreparedHistogramOnset::prepare(std::size_t bins,HistogramOnsetConfig c) {
    std::size_t bytes=0;if(!storage_bytes(bins,c,bytes))return false;
    PreparedHistogramOnset next;if(!next.histogram_.prepare(c.histogram))return false;
    next.config_=c;next.magnitude_.resize(bins);next.phase_.resize(bins);next.previous_magnitude_.resize(bins);
    next.previous_phase_.resize(bins);next.older_phase_.resize(bins);next.values_.resize(bins);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedHistogramOnset::reset() noexcept {
    std::fill(previous_magnitude_.begin(),previous_magnitude_.end(),0.f);
    std::fill(previous_phase_.begin(),previous_phase_.end(),0.f);std::fill(older_phase_.begin(),older_phase_.end(),0.f);
}
bool PreparedHistogramOnset::polar(std::span<const float> input) noexcept {
    if(magnitude_.empty() || input.size()!=2*magnitude_.size())return false;
    for(std::size_t bin=0;bin<magnitude_.size();++bin){
        const float re=input[2*bin],im=input[2*bin+1];if(!std::isfinite(re) || !std::isfinite(im))return false;
        magnitude_[bin]=std::hypot(re,im);phase_[bin]=(re!=0 || im!=0)?std::atan2(im,re):0;
        if(!std::isfinite(magnitude_[bin]))return false;
    }
    return true;
}
void PreparedHistogramOnset::phase_difference() noexcept {
    for(std::size_t bin=0;bin<values_.size();++bin){
        const float phase=float(double(phase_[bin])-2.*double(previous_phase_[bin])+double(older_phase_[bin]));
        values_[bin]=magnitude_[bin]>config_.magnitude_threshold?std::abs(source_unwrap_phase(phase)):0;
    }
}
void PreparedHistogramOnset::magnitude_difference() noexcept {
    for(std::size_t bin=0;bin<values_.size();++bin){
        const float difference=magnitude_[bin]*magnitude_[bin]-previous_magnitude_[bin]*previous_magnitude_[bin];
        values_[bin]=magnitude_[bin]>config_.magnitude_threshold?std::sqrt(std::abs(difference)):0;
    }
}
void PreparedHistogramOnset::commit() noexcept {
    std::copy(previous_phase_.begin(),previous_phase_.end(),older_phase_.begin());
    std::copy(phase_.begin(),phase_.end(),previous_phase_.begin());std::copy(magnitude_.begin(),magnitude_.end(),previous_magnitude_.begin());
}
bool PreparedHistogramOnset::evaluate(float& output) noexcept {
    if(config_.method==HistogramOnsetMethod::SpectralDifference)magnitude_difference();else phase_difference();
    float result=0;if(!histogram_.process(values_,result))return false;
    if(config_.method==HistogramOnsetMethod::WeightedPhase){
        for(std::size_t bin=0;bin<values_.size();++bin)values_[bin]*=magnitude_[bin];
        if(!histogram_.process(values_,result))return false;
    }
    commit();output=result;return true;
}
bool PreparedHistogramOnset::process(std::span<const float> input,float& output) noexcept {
    return polar(input) && evaluate(output);
}
bool PreparedHistogramOnset::process_polar(std::span<const float> magnitude,std::span<const float> phase,float& output) noexcept {
    if(config_.method==HistogramOnsetMethod::SpectralDifference)return process_magnitude(magnitude,output);
    if(magnitude_.empty() || magnitude.size()!=magnitude_.size() || phase.size()!=magnitude.size())return false;
    for(std::size_t i=0;i<magnitude.size();++i)
        if(!std::isfinite(magnitude[i]) || magnitude[i]<0 || !std::isfinite(phase[i]))return false;
    std::copy(magnitude.begin(),magnitude.end(),magnitude_.begin());std::copy(phase.begin(),phase.end(),phase_.begin());
    return evaluate(output);
}
bool PreparedHistogramOnset::process_magnitude(std::span<const float> magnitude,float& output) noexcept {
    if(config_.method!=HistogramOnsetMethod::SpectralDifference || magnitude_.empty() || magnitude.size()!=magnitude_.size() ||
       !std::all_of(magnitude.begin(),magnitude.end(),[](float value){return std::isfinite(value) && value>=0;}))return false;
    std::copy(magnitude.begin(),magnitude.end(),magnitude_.begin());return evaluate(output);
}
}
