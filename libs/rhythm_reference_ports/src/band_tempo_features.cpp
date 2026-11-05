// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TempoScaleBands at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/band_tempo_features.h>
#include <dsp_kernels/log_compression.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
float tempo_feature_scale(float value,float lower,float upper,float exponent) noexcept {
    if(value>upper)return upper+.1f*std::log10(value/upper);
    if(value>lower)return upper+(upper-lower)*std::pow((value-lower)/(upper-lower),exponent);
    return value;
}
bool PreparedBandTempoFeatures::prepare(std::span<const float> gains,float frame_time) {
    std::size_t admitted=0;if(!storage_bytes(gains,frame_time,admitted))return false;
    if(gains.empty() || !std::isfinite(frame_time) || frame_time<=0 ||
       !std::all_of(gains.begin(),gains.end(),[](float x){return std::isfinite(x);}))return false;
    const float factor=std::sqrt(256.f/frame_time);
    if(!std::isfinite(factor))return false;
    PreparedBandTempoFeatures next;next.frame_factor_=factor;
    next.gains_.assign(gains.begin(),gains.end());next.previous_.resize(gains.size());
    next.current_.resize(gains.size());next.result_.resize(gains.size());
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedBandTempoFeatures::reset() noexcept {
    std::fill(previous_.begin(),previous_.end(),0.f);
}
bool PreparedBandTempoFeatures::differences(float& cumulative) noexcept {
    float sum=0;
    const float normalization=std::log10(101.f);
    for(std::size_t i=0;i<current_.size();++i){
        current_[i]/=normalization;
        const float delta=std::max(0.f,current_[i]-previous_[i])*frame_factor_;
        sum+=delta;result_[i]=tempo_feature_scale(delta,.1f,.5f,.4f)*gains_[i];
        if(!std::isfinite(result_[i]))return false;
    }
    cumulative=tempo_feature_scale(sum,.2f,1.2f,.3f);return std::isfinite(cumulative);
}
bool PreparedBandTempoFeatures::process(std::span<const float> bands,std::span<float> output,float& cumulative) noexcept {
    if(gains_.empty() || bands.size()!=gains_.size() || output.size()!=gains_.size())return false;
    if(!dsp_kernels::logarithmic_compression(bands,current_,100,1))return false;
    float next=0;
    if(!differences(next))return false;
    previous_.swap(current_);std::copy(result_.begin(),result_.end(),output.begin());cumulative=next;return true;
}
}
