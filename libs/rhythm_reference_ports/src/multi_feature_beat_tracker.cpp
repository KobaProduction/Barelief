// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BeatTrackerMultiFeature's candidate/selection composition at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/multi_feature_beat_tracker.h>
#include <algorithm>
#include <utility>
namespace rhythm_reference_ports {
bool PreparedMultiFeatureBeatTracker::prepare(const std::array<std::size_t,5>& frames,MultiFeaturePeriodConfig config) {
    std::size_t admitted=0;if(!storage_bytes(frames,config,admitted))return false;
    PreparedMultiFeatureBeatTracker next;if(!next.configure(frames,config))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedMultiFeatureBeatTracker::configure(const std::array<std::size_t,5>& frames,MultiFeaturePeriodConfig config) {
    std::size_t maximum=0;
    for(std::size_t i=0;i<trackers_.size();++i){
        PeriodBeatTrackingConfig branch;branch.frame_rate=config.frame_rates[i];branch.upsampling=config.upsampling[i];
        branch.min_bpm=config.minimum_bpm;branch.max_bpm=config.maximum_bpm;branch.noise=config.noise;
        if(!trackers_[i].prepare(frames[i],branch))return false;
        const auto samples=trackers_[i].output_samples(frames[i]);masks_[i].resize(samples);times_[i].resize(samples);
        maximum=std::max(maximum,samples);
    }
    prepared_=agreement_.prepare(trackers_.size(),maximum,config.agreement_minimum_seconds);return prepared_;
}
bool PreparedMultiFeatureBeatTracker::candidate(std::size_t index,std::span<const float> values) noexcept {
    const auto samples=trackers_[index].output_samples(values.size());
    if(samples>masks_[index].size() || !trackers_[index].process(values,std::span(masks_[index]).first(samples)))return false;
    return trackers_[index].beat_times(times_[index],counts_[index]);
}
bool PreparedMultiFeatureBeatTracker::process(const std::array<OnsetCurveView,5>& curves) noexcept {
    ready_=false;counts_.fill(0);selection_={};
    if(!prepared_)return false;
    std::array<BeatSequenceView,5> views;
    for(std::size_t i=0;i<curves.size();++i){
        if(!candidate(i,curves[i].values))return false;
        views[i]={std::span(times_[i]).first(counts_[i])};
    }
    ready_=agreement_.process(views,selection_);return ready_;
}
std::span<const float> PreparedMultiFeatureBeatTracker::candidate_times(std::size_t method) const noexcept {
    return ready_ && method<times_.size()?std::span<const float>(times_[method]).first(counts_[method]):std::span<const float>{};
}
std::span<const float> PreparedMultiFeatureBeatTracker::ticks() const noexcept {
    return candidate_times(selection_.selected);
}
}
