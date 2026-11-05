// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BeatsLoudness/Slicer at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/beat_loudness_sequence.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
void slice_positions(float tick,BeatLoudnessSequenceConfig config,float& begin,float& length) noexcept {
    float start=tick-config.search_seconds/2.f;
    float end=tick+config.search_seconds/2.f+config.energy_seconds+.001f;
    if(start<0){const float adjustment=-start;start+=adjustment;end+=adjustment;}
    begin=start*config.sample_rate+.5f;length=(end-start)*config.sample_rate+.5f;
}
}
bool PreparedBeatLoudnessSequence::configure(std::size_t maximum,BeatLoudnessSequenceConfig config,std::span<const float> borders,const dsp_kernels::FourierBackendFactory* factory) {
    if(!maximum)return false;
    BeatSegmentEnergyConfig beat;beat.sample_rate=config.sample_rate;beat.search_seconds=config.search_seconds;
    beat.energy_seconds=config.energy_seconds;beat.window_precision=config.window_precision;
    if(!beat_.prepare(beat,borders,factory) || (beat_.bands() && maximum>SIZE_MAX/beat_.bands()))return false;
    config_=config;slices_.resize(maximum);energies_.resize(maximum);indices_.resize(maximum);
    ratios_.resize(maximum*beat_.bands());return true;
}
bool PreparedBeatLoudnessSequence::prepare(std::size_t maximum,BeatLoudnessSequenceConfig config,std::span<const float> borders,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,borders,bound,factory))return false;
    PreparedBeatLoudnessSequence next;
    if(!next.configure(maximum,config,borders,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatLoudnessSequence::slice(float tick,std::size_t source,Slice& result) const noexcept {
    if(!std::isfinite(tick))return false;
    float begin=0,length=0;slice_positions(tick,config_,begin,length);
    const float limit=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(begin) || !std::isfinite(length) || begin<0 || length<0 || begin>limit || length>limit)return false;
    const auto first=std::size_t(begin),count=std::size_t(length);
    if(count>std::size_t(std::numeric_limits<int>::max())-first)return false;
    result={first,first+count,source};return true;
}
bool PreparedBeatLoudnessSequence::schedule(std::span<const float> ticks) noexcept {
    if(ticks.size()>slices_.size())return false;
    for(std::size_t i=0;i<ticks.size();++i)if(!slice(ticks[i],i,slices_[i]))return false;
    scheduled_=ticks.size();
    std::sort(slices_.begin(),slices_.begin()+std::ptrdiff_t(scheduled_),[](const Slice& left,const Slice& right){
        if(left.begin!=right.begin)return left.begin<right.begin;
        return left.end==right.end?left.source<right.source:left.end<right.end;
    });
    return true;
}
bool PreparedBeatLoudnessSequence::analyze(std::span<const float> pcm) noexcept {
    for(std::size_t i=0;i<scheduled_;++i){
        const auto slice=slices_[i];
        if(slice.begin>=pcm.size() || slice.end>pcm.size())break;
        BeatSegmentEnergyResult result;
        auto ratios=std::span(ratios_).subspan(count_*beat_.bands(),beat_.bands());
        if(!beat_.process(pcm.subspan(slice.begin,slice.end-slice.begin),result,ratios))return false;
        energies_[count_]=result.energy;indices_[count_]=slice.source;++count_;
    }
    return true;
}
bool PreparedBeatLoudnessSequence::process(std::span<const float> pcm,std::span<const float> ticks) noexcept {
    ready_=false;count_=0;scheduled_=0;
    if(slices_.empty() || pcm.empty() || pcm.size()>std::size_t(std::numeric_limits<int>::max()) ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    if(!schedule(ticks) || !analyze(pcm))return false;
    ready_=true;return true;
}
std::span<const float> PreparedBeatLoudnessSequence::energies() const noexcept {
    return ready_?std::span<const float>(energies_).first(count_):std::span<const float>{};
}
std::span<const float> PreparedBeatLoudnessSequence::band_ratios(std::size_t row) const noexcept {
    if(!ready_ || row>=count_)return {};
    return std::span(ratios_).subspan(row*beat_.bands(),beat_.bands());
}
std::span<const std::size_t> PreparedBeatLoudnessSequence::source_beat_indices() const noexcept {
    return ready_?std::span<const std::size_t>(indices_).first(count_):std::span<const std::size_t>{};
}
}
