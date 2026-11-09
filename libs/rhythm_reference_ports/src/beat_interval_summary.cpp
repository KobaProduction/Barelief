// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia RhythmExtractor2013 aggregation at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/beat_interval_summary.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <dsp_kernels/prepared_storage.h>

namespace rhythm_reference_ports {
bool PreparedBeatIntervalSummary::prepare(std::size_t maximum,float maximum_bpm) {
    std::size_t bytes=0;if(!storage_bytes(maximum,maximum_bpm,bytes))return false;
    PreparedBeatIntervalSummary next;
    LagTempoVoteConfig vote;vote.sample_rate=1;vote.audio_hop=1;vote.maximum_bpm=maximum_bpm;
    if(!next.vote_.prepare(std::max(std::size_t{1},maximum-1),vote))return false;
    next.maximum_ticks_=maximum;next.intervals_.resize(maximum-1);if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatIntervalSummary::storage_bytes(std::size_t maximum,float maximum_bpm,std::size_t& output) noexcept {
    if(!maximum)return false;
    LagTempoVoteConfig vote;vote.sample_rate=1;vote.audio_hop=1;vote.maximum_bpm=maximum_bpm;
    std::size_t bytes=0;
    if(!PreparedLagTempoVote::storage_bytes(std::max(std::size_t{1},maximum-1),vote,bytes) ||
       !dsp_kernels::prepared_storage::add_array(bytes,maximum-1,sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedBeatIntervalSummary::measure_intervals(std::span<const float> ticks) noexcept {
    for(std::size_t i=1;i<ticks.size();++i){
        const float interval=ticks[i]-ticks[i-1];
        if(!std::isfinite(interval) || interval<=0)return false;
        intervals_[i-1]=interval;
    }
    return true;
}
bool PreparedBeatIntervalSummary::process(std::span<const float> ticks,float confidence) noexcept {
    ready_=false;count_=0;confidence_=0;
    if(!maximum_ticks_ || ticks.size()>maximum_ticks_ || !std::isfinite(confidence) ||
       !std::all_of(ticks.begin(),ticks.end(),[](float x){return std::isfinite(x);}))return false;
    if(!measure_intervals(ticks))return false;
    const auto count=ticks.empty()?0:ticks.size()-1;
    if(!vote_.process(std::span(intervals_).first(count)))return false;
    count_=count;confidence_=confidence;ready_=true;return true;
}
std::span<const float> PreparedBeatIntervalSummary::intervals() const noexcept {
    return ready_?std::span<const float>(intervals_).first(count_):std::span<const float>{};
}
std::span<const float> PreparedBeatIntervalSummary::estimates() const noexcept {
    return ready_?vote_.estimates():std::span<const float>{};
}
bool PreparedBeatIntervalSummary::histogram_descriptors(BeatIntervalHistogram& output) const noexcept {
    return ready_ && beat_interval_histogram(intervals(),output);
}
}
