// Adapted from madmom features/onsets.py correlation_diff at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/spectral_alignment.h>
#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedSpectralAlignment::prepare(std::size_t bins,std::size_t radius,bool positive) {
    if(!radius || bins>std::size_t(PTRDIFF_MAX)/2 || radius>=(bins+1)/2)return false;
    PreparedSpectralAlignment next;next.bins_=bins;next.radius_=radius;next.positive_=positive;
    next.correlations_.resize(2*radius+1);next.difference_.resize(bins);
    *this=std::move(next);return true;
}
bool PreparedSpectralAlignment::process(std::span<const float> current,std::span<const float> reference,std::span<float> output) noexcept {
    if(!bins_ || current.size()!=bins_ || reference.size()!=bins_ || output.size()!=bins_)return false;
    if(!linear_crosscorrelation(current,reference,-std::ptrdiff_t(radius_),correlations_,CorrelationNormalization::Raw))return false;
    const auto peak=std::size_t(std::max_element(correlations_.begin(),correlations_.end())-correlations_.begin());
    const auto offset=std::ptrdiff_t(radius_)-std::ptrdiff_t(peak);
    std::fill(difference_.begin(),difference_.end(),0.f);
    for(std::size_t i=radius_;i<bins_-radius_;++i){
        const float value=current[i]-reference[std::size_t(std::ptrdiff_t(i)+offset)];
        if(!std::isfinite(value))return false;
        difference_[i]=positive_?std::max(0.f,value):value;
    }
    std::copy(difference_.begin(),difference_.end(),output.begin());offset_=offset;return true;
}
}
