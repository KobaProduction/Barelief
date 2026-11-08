// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of OnsetDetectionGlobal at MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/global_onset.h>
#include <dsp_kernels/sequence_statistics.h>
#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace rhythm_reference_ports {
namespace {
bool nonnegative(std::span<const float> x) noexcept {
    return std::all_of(x.begin(),x.end(),[](float v){return std::isfinite(v) && v>=0;});
}
}
bool PreparedInformationGain::prepare(std::size_t bins,std::size_t first,std::size_t stop) {
    std::size_t admitted=0;if(!storage_bytes(bins,first,stop,admitted))return false;
    if(!bins || first>=stop || stop>bins || stop-first>SIZE_MAX/11)return false;
    PreparedInformationGain next;next.bins_=bins;next.first_=first;next.count_=stop-first;
    next.history_.assign(11*next.count_,0);if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedInformationGain::reset() noexcept {
    std::fill(history_.begin(),history_.end(),0);next_=0;
}
float PreparedInformationGain::bin_change(std::size_t bin) const noexcept {
    float old=0,recent=0;
    for(std::size_t i=0;i<5;++i){
        const float weight=1.f-float(i)*.9f/5.f;
        const float old_weight=1.f-float(4-i)*.9f/5.f;
        old+=history_[((next_+i)%11)*count_+bin]*old_weight;
        recent+=history_[((next_+6+i)%11)*count_+bin]*weight;
    }
    if(old==0)old=1;
    if(recent==0)recent=std::numeric_limits<float>::epsilon();
    return std::max(std::log2(recent/old),0.f);
}
bool PreparedInformationGain::process(std::span<const float> magnitude,float& output) noexcept {
    if(!count_ || magnitude.size()!=bins_ || !nonnegative(magnitude))return false;
    std::copy_n(magnitude.begin()+std::ptrdiff_t(first_),count_,history_.begin()+std::ptrdiff_t(next_*count_));
    next_=(next_+1)%11;float value=0;
    for(std::size_t i=0;i<count_;++i)value+=bin_change(i);
    if(!std::isfinite(value))return false;
    output=value;return true;
}
}
