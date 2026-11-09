// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogram at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/frame_tempo_path.h>
#include <rhythm_reference_ports/harmonic_tempo.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool finite_range(float value,float minimum) noexcept {
    return std::isfinite(value) && value>=minimum;
}
float fold_tempo(float value,float reference) noexcept {
    const float ratio=value/reference;
    return ratio<1?value*std::round(1.f/ratio):value/std::round(ratio);
}
bool valid_peak(const InterpolatedPeak& peak) noexcept {
    return finite_range(peak.position,0) && std::isfinite(peak.value);
}
}
bool PreparedFrameTempoPath::configure(std::size_t frames,FrameTempoPathConfig config) {
    if(!frames || !config.hop || !finite_range(config.frequency_step,std::numeric_limits<float>::min()) ||
       !finite_range(config.frame_rate,1) || !finite_range(config.minimum_bpm,0) ||
       !finite_range(config.maximum_bpm,std::numeric_limits<float>::min()) || config.minimum_bpm>config.maximum_bpm)return false;
    if(!finite_range(config.tolerance_percent,0) || !finite_range(config.minimum_change_seconds,0) ||
       !finite_range(config.induced_bpm,0))return false;
    const float stable=config.minimum_change_seconds*config.frame_rate/float(config.hop);
    const float maximum=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(stable) || stable>maximum || !std::isfinite(config.frequency_step*60.f))return false;
    config_=config;stable_frames_=std::size_t(stable);bpm_ratio_=config.frequency_step*60.f;
    bpms_.resize(frames);mean_bpm_=config.induced_bpm;reset();return true;
}
bool PreparedFrameTempoPath::prepare(std::size_t frames,FrameTempoPathConfig config) {
    std::size_t bound=0;if(!storage_bytes(frames,config,bound))return false;
    PreparedFrameTempoPath next;
    if(!next.configure(frames,config) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedFrameTempoPath::reset(bool reset_reference) noexcept {
    count_=0;status_=FrameTempoPathStatus::Unprocessed;
    if(reset_reference)mean_bpm_=config_.induced_bpm;
}
float PreparedFrameTempoPath::equal_bin(std::span<const InterpolatedPeak> peaks,float main) const noexcept {
    for(const auto& peak:peaks)
        if(approximately_equal_tempo(main,peak.position*bpm_ratio_,config_.tolerance_percent))return peak.position;
    return -1;
}
bool PreparedFrameTempoPath::harmonic_match(float main,float candidate) const noexcept {
    const bool harmonic=harmonically_related_tempo(main,candidate,config_.tolerance_percent);
    if(!config_.constant_tempo && harmonic)return true;
    if(config_.constant_tempo && !harmonic)return false;
    float common=0;
    return greatest_common_tempo(main,candidate,config_.tolerance_percent,common) && common>config_.minimum_bpm;
}
float PreparedFrameTempoPath::harmonic_bin(std::span<const InterpolatedPeak> peaks,float main) const noexcept {
    for(const auto& peak:peaks){
        const float candidate=peak.position*bpm_ratio_;
        if(candidate<config_.minimum_bpm || candidate>config_.maximum_bpm || !harmonic_match(main,candidate))continue;
        if(!config_.constant_tempo)return peak.position;
        const float ratio=main/candidate;
        return ratio<1?peak.position/std::round(1.f/ratio):peak.position*std::round(ratio);
    }
    return -1;
}
float PreparedFrameTempoPath::nearby_bin(std::span<const InterpolatedPeak> peaks,float main,float last) const noexcept {
    for(const auto& peak:peaks){
        if(peak.position==0)continue;
        float ratio=last>0?last/peak.position:main/(peak.position*bpm_ratio_);
        if(ratio<1)ratio=1.f/ratio;
        if(ratio<=2)return peak.position;
    }
    return 0;
}
bool PreparedFrameTempoPath::select_peak(std::span<const InterpolatedPeak> peaks,float main,float& last,float& bpm) const noexcept {
    if(!std::all_of(peaks.begin(),peaks.end(),valid_peak))return false;
    if(peaks.empty()){bpm=0;return true;}
    float bin=equal_bin(peaks,main);
    if(bin<0)bin=harmonic_bin(peaks,main);
    if(bin<0)bin=nearby_bin(peaks,main,last);
    last=bin;bpm=bin*bpm_ratio_;
    return std::isfinite(bpm);
}
void PreparedFrameTempoPath::matching_tempi(float main,float& sum,std::size_t& matches) const noexcept {
    for(std::size_t i=0;i<count_;++i)if(approximately_equal_tempo(main,bpms_[i],config_.tolerance_percent)){
        sum+=bpms_[i];++matches;
    }
}
bool PreparedFrameTempoPath::select(SpectralPeakFrames source,std::size_t frames,float main) noexcept {
    float last=-1;
    for(std::size_t i=0;i<frames;++i){
        const auto peaks=source.read(source.context,i);
        if(!select_peak(peaks,main,last,bpms_[i]))return false;
    }
    count_=frames;return true;
}
bool PreparedFrameTempoPath::learn_mean(float main) noexcept {
    if(mean_bpm_>0)return true;
    float sum=0;std::size_t matches=0;
    matching_tempi(main,sum,matches);
    if(!matches){status_=FrameTempoPathStatus::NoMatchingTempo;return true;}
    if(!std::isfinite(sum))return false;
    mean_bpm_=sum/float(matches);return true;
}
std::size_t PreparedFrameTempoPath::run_end(std::size_t index) const noexcept {
    auto j=index+1;
    // Original: areEqual(bpms[i],bpms[j], _bpmTolerance && bpms[j]!=0).
    while(j<count_ && approximately_equal_tempo(bpms_[index],bpms_[j],
          float(config_.tolerance_percent!=0 && bpms_[j]!=0)))++j;
    return j;
}
bool PreparedFrameTempoPath::postprocess() noexcept {
    for(std::size_t i=0;i<count_;++i){
        if(bpms_[i]==0 || approximately_equal_tempo(mean_bpm_,bpms_[i],config_.tolerance_percent))continue;
        const auto end=run_end(i);
        if(end-i-1>=stable_frames_){i=end;continue;}
        bpms_[i]=harmonically_related_tempo(bpms_[i],mean_bpm_,config_.tolerance_percent)?
            fold_tempo(bpms_[i],mean_bpm_):mean_bpm_;
        if(!std::isfinite(bpms_[i]))return false;
    }
    return true;
}
bool PreparedFrameTempoPath::process(SpectralPeakFrames source,std::size_t frames,float main) noexcept {
    count_=0;status_=FrameTempoPathStatus::Unprocessed;
    if(bpms_.empty() || frames>bpms_.size() || !finite_range(main,0))return false;
    if(main==0){status_=FrameTempoPathStatus::NoReference;return true;}
    if(!source.read || !select(source,frames,main) || !learn_mean(main))return false;
    if(status_==FrameTempoPathStatus::NoMatchingTempo)return true;
    if(!postprocess())return false;
    status_=FrameTempoPathStatus::Ready;return true;
}
}
