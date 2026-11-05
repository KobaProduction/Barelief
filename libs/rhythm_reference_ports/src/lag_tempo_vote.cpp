// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia RhythmExtractor postprocessing at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/lag_tempo_vote.h>
#include <dsp_kernels/value_histogram.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedLagTempoVote::prepare(std::size_t maximum,LagTempoVoteConfig config) {
    std::size_t admitted=0;if(!storage_bytes(maximum,config,admitted))return false;
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!maximum || !config.audio_hop || !std::isfinite(config.sample_rate) || config.sample_rate<=0 ||
       !std::isfinite(config.maximum_bpm) || config.maximum_bpm<=0 || config.maximum_bpm>bound)return false;
    PreparedLagTempoVote next;next.config_=config;
    next.bpms_.resize(maximum);next.estimates_.resize(maximum);
    next.counts_.resize(std::size_t(config.maximum_bpm/2.f+.5f)+1);if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedLagTempoVote::convert(dsp_kernels::FloatSequenceSource lags,std::size_t size,std::size_t& count) noexcept {
    count=0;
    for(std::size_t i=0;i<size;++i){
        float lag=0;
        if(!lags.read(lags.context,i,lag) || !std::isfinite(lag))return false;
        if(lag==0)continue;
        const float bpm=60.f*config_.sample_rate/lag/float(config_.audio_hop);
        if(!std::isfinite(bpm) || bpm>config_.maximum_bpm)return false;
        bpms_[count++]=bpm/2.f;
    }
    return true;
}
bool PreparedLagTempoVote::select(std::size_t observations) noexcept {
    auto bpms=std::span(bpms_).first(observations);
    if(!dsp_kernels::rounded_bin_counts(bpms,counts_))return false;
    const float closest=float(std::max_element(counts_.begin(),counts_.end())-counts_.begin())*2.f;
    float sum=0;
    for(float bpm:bpms){
        bpm*=2.f;
        if(std::abs(closest-bpm)<5.f){estimates_[count_++]=bpm;sum+=bpm;}
    }
    bpm_=count_?sum/float(count_):closest;return std::isfinite(bpm_);
}
bool PreparedLagTempoVote::process(std::span<const float> lags) noexcept {
    const auto read=[](const void* context,std::size_t at,float& value) noexcept {
        const auto values=*static_cast<const std::span<const float>*>(context);
        if(at>=values.size())return false;
        value=values[at];return true;
    };
    return process({&lags,read},lags.size());
}
bool PreparedLagTempoVote::process(dsp_kernels::FloatSequenceSource lags,std::size_t size) noexcept {
    bpm_=0;count_=0;
    if(bpms_.empty() || size>bpms_.size() || !lags.read)return false;
    std::size_t observations=0;
    if(!convert(lags,size,observations))return false;
    if(!observations)return true;
    if(!select(observations)){bpm_=0;count_=0;return false;}
    return true;
}
}
