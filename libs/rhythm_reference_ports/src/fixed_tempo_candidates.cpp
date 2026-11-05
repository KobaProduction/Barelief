// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia NoveltyCurveFixedBpmEstimator at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/fixed_tempo_candidates.h>
#include <dsp_kernels/order_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool valid(const FixedTempoCandidateConfig& config) noexcept {
    return std::isfinite(config.sample_rate) && config.sample_rate>=1 && std::isfinite(60.f*config.sample_rate) &&
        config.hop_samples && std::isfinite(config.minimum_bpm) && std::isfinite(config.maximum_bpm) &&
        config.minimum_bpm>0 && config.minimum_bpm<config.maximum_bpm && std::isfinite(config.tolerance_percent) &&
        config.tolerance_percent>0 && config.tolerance_percent<=100;
}

}
float PreparedFixedTempoCandidates::lag_to_bpm(float lag) const noexcept {
    return 60.f*config_.sample_rate/lag/float(config_.hop_samples);
}
float PreparedFixedTempoCandidates::bpm_to_lag(float bpm) const noexcept {
    return lag_to_bpm(bpm);
}
bool PreparedFixedTempoCandidates::prepare_detectors() {
    InterpolatedPeakConfig config;
    config.range=float(frames_-1);config.maximum=config.range;config.minimum=float(minimum_lag_);
    config.threshold=0;config.maximum_peaks=frames_;
    if(!initial_.prepare(frames_,config))return false;
    config.range=float(frames_);config.maximum=config.range;
    return iteration_.prepare(frames_,config);
}
bool PreparedFixedTempoCandidates::configure(std::size_t frames,FixedTempoCandidateConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(frames<3 || !valid(config))return false;
    config_=config;frames_=frames;
    const float lag=std::round(bpm_to_lag(config.maximum_bpm));
    if(!std::isfinite(lag) || lag<1 || lag>=float(frames-1))return false;
    minimum_lag_=std::size_t(lag);
    dsp_kernels::SpectralAutocorrelationConfig correlation_config;
    correlation_config.normalization=dsp_kernels::CorrelationNormalization::Unbiased;
    if(!correlation_.prepare(frames,correlation_config,factory) || !average_.prepare(minimum_lag_))return false;
    if(!prepare_detectors() || !distribution_.prepare())return false;
    allocate(frames);return true;
}
void PreparedFixedTempoCandidates::allocate(std::size_t frames) {
    correlation_values_.resize(frames);smoothed_.resize(frames);scratch_.resize(frames);selected_.resize(frames);
}
bool PreparedFixedTempoCandidates::prepare(std::size_t frames,FixedTempoCandidateConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(frames,config,bound,factory))return false;
    PreparedFixedTempoCandidates next;
    if(!next.configure(frames,config,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
std::size_t PreparedFixedTempoCandidates::prominent_peaks() noexcept {
    const auto peaks=initial_.peaks();
    for(std::size_t i=0;i<peaks.size();++i)scratch_[i]=peaks[i].value;
    auto amplitudes=std::span(scratch_).first(peaks.size());
    const float mean=std::accumulate(amplitudes.begin(),amplitudes.end(),0.f)/float(peaks.size());
    const float threshold=.1f*std::min(dsp_kernels::median_in_place(amplitudes),mean);
    std::size_t count=0;
    for(const auto peak:peaks)if(peak.value>=threshold)selected_[count++]=peak;
    return count;
}
std::size_t PreparedFixedTempoCandidates::period_bpms(std::span<const InterpolatedPeak> peaks,bool strict) noexcept {
    std::size_t count=0;
    for(std::size_t i=1;i<peaks.size();++i){
        const float lag=std::abs(peaks[i].position-peaks[i-1].position);
        if(lag<=0)continue;
        const float bpm=std::round(lag_to_bpm(lag));
        const bool inside=strict?bpm>config_.minimum_bpm && bpm<config_.maximum_bpm:
            bpm>=config_.minimum_bpm && bpm<=config_.maximum_bpm;
        if(inside)scratch_[count++]=bpm;
    }
    return count;
}
bool PreparedFixedTempoCandidates::tatum(float& width) noexcept {
    selected_count_=prominent_peaks();
    const auto count=period_bpms(std::span(selected_).first(selected_count_),true);
    if(!distribution_.process(std::span(scratch_).first(count)))return false;
    distribution_.sort_by_magnitude();
    const auto peaks=distribution_.peaks();
    if(peaks.empty() || peaks[0].position<=0)return false;
    width=4.f*std::round(bpm_to_lag(peaks[0].position));return std::isfinite(width) && width>0;
}
std::size_t PreparedFixedTempoCandidates::nearest(std::span<const InterpolatedPeak> peaks,float value) const noexcept {
    std::size_t best=0;
    float distance=std::numeric_limits<float>::infinity();
    for(std::size_t i=0;i<peaks.size();++i){
        const float current=std::abs(peaks[i].position-value);
        if(current<distance){distance=current;best=i;}
    }
    return best;
}
std::size_t PreparedFixedTempoCandidates::local_maximum(std::span<const InterpolatedPeak> peaks,std::size_t i,float width) const noexcept {
    const float first=std::trunc(std::max(0.f,peaks[i].position-width));
    const float last=std::trunc(std::min(float(frames_),peaks[i].position+width+.5f));
    const auto begin=nearest(peaks,first),end=nearest(peaks,last);
    auto best=begin;
    for(auto j=begin;j<=end;++j)if(peaks[j].value>peaks[best].value)best=j;
    return best;
}
bool PreparedFixedTempoCandidates::initial_threshold() noexcept {
    float width=0;
    if(!tatum(width))return false;
    return threshold_from_local_peaks(width);
}
bool PreparedFixedTempoCandidates::threshold_from_local_peaks(float width) noexcept {
    const auto peaks=std::span(selected_).first(selected_count_);
    std::size_t count=0,previous=std::numeric_limits<std::size_t>::max();
    float sum=0;
    for(std::size_t i=0;i<peaks.size();++i){
        const auto best=local_maximum(peaks,i,width);
        if(best==previous || peaks[best].value<0)continue;
        sum+=peaks[best].value;previous=best;++count;
    }
    if(!count)return false;
    threshold_=sum/float(count);return std::isfinite(threshold_);
}
void PreparedFixedTempoCandidates::copy_candidates() noexcept {
    const auto peaks=distribution_.peaks();
    std::copy(peaks.begin(),peaks.end(),result_.begin());result_count_=peaks.size();
}
bool PreparedFixedTempoCandidates::refine() noexcept {
    refinements_=0;
    for(;;){
        if(!refine_distribution())return false;
        if(result_count_>2){status_=FixedTempoCandidateStatus::Ready;return true;}
        const float next=threshold_*.5f;
        if(next==threshold_){status_=FixedTempoCandidateStatus::ThresholdExhausted;return true;}
        threshold_=next;
    }
}
bool PreparedFixedTempoCandidates::refine_distribution() noexcept {
    if(!iteration_.set_threshold(threshold_) || !iteration_.process(smoothed_))return false;
    const auto count=period_bpms(iteration_.peaks(),false);
    if(!distribution_.process(std::span(scratch_).first(count)) || !distribution_.merge(config_.tolerance_percent))return false;
    copy_candidates();++refinements_;return true;
}
void PreparedFixedTempoCandidates::invalidate() noexcept {
    ready_=false;status_=FixedTempoCandidateStatus::Failed;
    result_count_=0;selected_count_=0;refinements_=0;threshold_=0;
}
bool PreparedFixedTempoCandidates::estimate() noexcept {
    if(initial_.peaks().empty()){status_=FixedTempoCandidateStatus::NoPeaks;return true;}
    if(!initial_threshold()){status_=FixedTempoCandidateStatus::NoTatum;return true;}
    return refine();
}
bool PreparedFixedTempoCandidates::process(std::span<const float> novelty) noexcept {
    invalidate();
    if(!frames_ || novelty.size()!=frames_)return false;
    average_.reset();
    if(!correlation_.process(novelty,correlation_values_) || !average_.process(correlation_values_,smoothed_) ||
       !initial_.process(smoothed_) || !estimate()){invalidate();return false;}
    ready_=true;return true;
}
}
