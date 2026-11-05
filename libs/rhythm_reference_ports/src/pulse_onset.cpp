// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia PercivalBpmEstimator's onset strength recipe.
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/pulse_onset.h>
#include <dsp_kernels/log_compression.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
namespace {
constexpr std::array<float,15> filter_numerator{
    .00933978f,.01521148f,.03163891f,.05607187f,.08390299f,.10948195f,.12742038f,
    .13386527f,.12742038f,.10948195f,.08390299f,.05607187f,.03163891f,.01521148f,.00933978f};
}
bool PreparedPulseOnset::configure(std::size_t bins,std::size_t size) {
    if(size<2 || bins!=size/2+1)return false;
    dsp_kernels::SpectralDescriptorConfig descriptor;
    descriptor.method=dsp_kernels::SpectralDescriptor::Flux;descriptor.aggregation=dsp_kernels::SpectralAggregation::Sum;
    descriptor.compensated_sum=false;
    std::array<float,15> denominator{};denominator[0]=1;
    if(!difference_.prepare(bins,descriptor) ||
       !filter_.prepare(filter_numerator,denominator,dsp_kernels::SubnormalStatePolicy::FlushAfterFirst))return false;
    fft_size_=size;scaled_.resize(bins);compressed_.resize(bins);return true;
}
bool PreparedPulseOnset::prepare(std::size_t bins,std::size_t size) {
    std::size_t bound=0;if(!storage_bytes(bins,size,bound))return false;
    PreparedPulseOnset next;
    if(!next.configure(bins,size) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedPulseOnset::reset() noexcept {
    difference_.reset();filter_.reset();
}
bool PreparedPulseOnset::compress(std::span<const float> magnitude) noexcept {
    if(!fft_size_ || magnitude.size()!=scaled_.size())return false;
    if(!std::all_of(magnitude.begin(),magnitude.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    const float scale=1.f/float(fft_size_);
    for(std::size_t i=0;i<magnitude.size();++i)scaled_[i]=magnitude[i]*scale;
    return dsp_kernels::logarithmic_compression(scaled_,compressed_,1000,1,dsp_kernels::LogarithmBase::Natural);
}
bool PreparedPulseOnset::process(std::span<const float> magnitude,PulseOnsetObservation& output) noexcept {
    if(!compress(magnitude))return false;
    PulseOnsetObservation next;
    if(!difference_.process_values(compressed_,next.novelty) || !filter_.process(next.novelty,next.filtered))return false;
    output=next;return true;
}
}
