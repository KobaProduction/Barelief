// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia Danceability at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/detrended_fluctuation.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedDetrendedFluctuation::prepare(std::span<const std::size_t> scales) {
    std::size_t bytes=0;
    if(scales.empty() || !std::is_sorted(scales.begin(),scales.end()) ||
       scales.front()<2 || scales.back()>std::size_t(std::numeric_limits<int>::max()) ||
       !required_workspace_bytes(scales.size(),bytes))return false;
    PreparedDetrendedFluctuation next;next.scales_.assign(scales.begin(),scales.end());
    next.fluctuations_.resize(scales.size());next.exponents_.resize(scales.size()-1);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedDetrendedFluctuation::scale(std::span<const float> values,std::size_t index) noexcept {
    const auto tau=scales_[index],jump=std::max(tau/50,std::size_t{1});
    float sum=0;
    for(std::size_t start=0;start<values.size()-tau;start+=jump){
        float residual=0;
        if(!dsp_kernels::linear_residual_mean_square(values.subspan(start,tau),residual))return false;
        sum+=residual;
    }
    const float value=values.size()==tau?0.f:std::sqrt(sum/(float(values.size()-tau)/float(jump)));
    if(!std::isfinite(value))return false;
    fluctuations_[index]=value;return true;
}
bool PreparedDetrendedFluctuation::publish_score(float sum) noexcept {
    const float alpha=sum/float(result_.available_scales-1);
    result_.score=alpha>0?1.f/alpha:0.f;
    if(!std::isfinite(alpha) || !std::isfinite(result_.score)){
        result_.score=0;result_.status=FluctuationStatus::NonFiniteExponent;return false;
    }
    result_.status=FluctuationStatus::Ok;
    return true;
}
bool PreparedDetrendedFluctuation::calculate_scales(std::span<const float> values) noexcept {
    for(std::size_t i=0;i<scales_.size() && scales_[i]<=values.size();++i){
        if(!scale(values,i)){result_.status=FluctuationStatus::NonFiniteFluctuation;return false;}
        ++result_.available_scales;
    }
    return true;
}
bool PreparedDetrendedFluctuation::score() noexcept {
    float sum=0;
    for(std::size_t i=0;i+1<result_.available_scales;++i){
        if(fluctuations_[i+1]==0){
            std::fill(exponents_.begin(),exponents_.end(),0.f);
            result_.status=FluctuationStatus::ZeroFluctuation;return true;
        }
        const float exponent=std::log10(fluctuations_[i+1]/fluctuations_[i])/
            std::log10((float(scales_[i+1])+3.f)/(float(scales_[i])+3.f));
        if(!std::isfinite(exponent)){result_.status=FluctuationStatus::NonFiniteExponent;return false;}
        exponents_[i]=exponent;sum+=exponent;
    }
    return publish_score(sum);
}
bool PreparedDetrendedFluctuation::process(std::span<const float> values) noexcept {
    result_={};std::fill(fluctuations_.begin(),fluctuations_.end(),0.f);std::fill(exponents_.begin(),exponents_.end(),0.f);
    if(scales_.empty() || values.size()>std::size_t(std::numeric_limits<int>::max()) ||
       !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    if(!calculate_scales(values))return false;
    if(result_.available_scales<2){result_.status=FluctuationStatus::TooShort;return true;}
    return score();
}
}
