// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogram::computeBpm at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/fourier_tempo_candidates.h>
#include <dsp_kernels/order_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool valid_configuration(std::size_t bins,const FourierTempoCandidateConfig& config) noexcept {
    if(bins<2 || !config.maximum_peaks || config.maximum_peaks==std::numeric_limits<std::size_t>::max())return false;
    if(!std::isfinite(config.frequency_step) || config.frequency_step<=0 || !std::isfinite(60.f*config.frequency_step))return false;
    if(!std::isfinite(config.minimum_bpm) || !std::isfinite(config.maximum_bpm) || config.minimum_bpm<0 || config.maximum_bpm<=config.minimum_bpm)return false;
    return double(config.maximum_bpm)<=double(std::numeric_limits<std::size_t>::max()/sizeof(float))-2;
}
}
bool PreparedFourierTempoCandidates::prepare_detector() {
    InterpolatedPeakConfig peaks;
    peaks.range=float(bins_-1);peaks.minimum=config_.minimum_bpm/(60.f*config_.frequency_step);
    peaks.maximum=config_.maximum_bpm/(60.f*config_.frequency_step);peaks.threshold=0;
    peaks.maximum_peaks=config_.maximum_peaks;peaks.ordering=PeakOrdering::Amplitude;
    return detector_.prepare(bins_,peaks);
}
bool PreparedFourierTempoCandidates::configure(std::size_t bins,FourierTempoCandidateConfig config) {
    if(!valid_configuration(bins,config))return false;
    config.minimum_bpm=std::floor(config.minimum_bpm);config.maximum_bpm=std::ceil(config.maximum_bpm);
    config_=config;bins_=bins;
    if(!prepare_detector())return false;
    scratch_.resize(bins);tempo_.resize(std::size_t(config.maximum_bpm)+1);observations_.resize(config.maximum_peaks+1);return true;
}
bool PreparedFourierTempoCandidates::prepare(std::size_t bins,FourierTempoCandidateConfig config) {
    std::size_t bound=0;if(!storage_bytes(bins,config,bound))return false;
    PreparedFourierTempoCandidates next;
    if(!next.configure(bins,config) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
float PreparedFourierTempoCandidates::threshold(std::span<const float> magnitude) noexcept {
    std::copy(magnitude.begin(),magnitude.end(),scratch_.begin());
    const float mean=std::accumulate(magnitude.begin(),magnitude.end(),0.f)/float(magnitude.size());
    return std::min(1e-6f,std::min(dsp_kernels::median_in_place(scratch_),mean));
}
bool PreparedFourierTempoCandidates::process(std::span<const float> magnitude) noexcept {
    if(magnitude.size()!=bins_ || !detector_.process(magnitude))return false;
    const float cutoff=threshold(magnitude);
    count_=0;std::fill(tempo_.begin(),tempo_.end(),0);
    for(const auto peak:detector_.peaks()){
        const float bpm=std::round(peak.position*(config_.frequency_step*60.f));
        if(peak.value<cutoff || bpm<config_.minimum_bpm || bpm>config_.maximum_bpm)continue;
        observations_[count_++]={bpm,peak.value};tempo_[std::size_t(bpm)]=peak.value;
    }
    if(!count_){observations_[0]={0,0};count_=1;}
    return true;
}
}
