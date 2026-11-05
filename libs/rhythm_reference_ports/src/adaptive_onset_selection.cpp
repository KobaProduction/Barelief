// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia Onsets::compute gate/adaptive threshold/cleanup at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/adaptive_onset_selection.h>
#include <dsp_kernels/order_statistics.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
bool valid(const AdaptiveOnsetSelectionConfig& c,std::size_t frames) noexcept {
    return frames && frames<SIZE_MAX && c.delay>=2 && std::isfinite(c.frame_rate) && c.frame_rate>0 &&
        std::isfinite(float(frames)/c.frame_rate) && std::isfinite(c.alpha) && c.alpha>=0 && c.alpha<=1 &&
        std::isfinite(c.silence_threshold) && c.silence_threshold>=0 && c.silence_threshold<=1;
}
}
bool PreparedAdaptiveOnsetSelection::prepare(std::size_t frames,AdaptiveOnsetSelectionConfig config) {
    std::size_t bytes=0;if(!storage_bytes(frames,config,bytes))return false;
    PreparedAdaptiveOnsetSelection next;next.maximum_frames_=frames;next.config_=config;
    next.gated_.resize(frames);next.threshold_.resize(frames);next.decisions_.resize(frames);next.result_.resize(frames);
    next.buffer_.resize(config.delay);next.sorted_.resize(config.delay);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedAdaptiveOnsetSelection::storage_bytes(std::size_t frames,AdaptiveOnsetSelectionConfig config,std::size_t& output) noexcept {
    std::size_t bytes=0;
    if(!valid(config,frames) || !dsp_kernels::prepared_storage::add_array(bytes,frames,2*sizeof(float)+2*sizeof(std::uint8_t)) ||
       !dsp_kernels::prepared_storage::add_array(bytes,config.delay,2*sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedAdaptiveOnsetSelection::gate(std::span<const float> input,float weights) noexcept {
    const float silence=config_.silence_threshold*weights;if(!std::isfinite(silence))return false;
    for(std::size_t i=0;i<input.size();++i){
        if(!std::isfinite(input[i]))return false;
        gated_[i]=input[i]<silence?0:input[i];
    }
    return true;
}
float PreparedAdaptiveOnsetSelection::threshold() noexcept {
    std::copy(buffer_.begin(),buffer_.end(),sorted_.begin());
    const float median=dsp_kernels::median_in_place(sorted_);float mean=0;
    if(!dsp_kernels::arithmetic_mean_float32(buffer_,mean))return std::numeric_limits<float>::quiet_NaN();
    return median+config_.alpha*mean;
}
bool PreparedAdaptiveOnsetSelection::decide(std::size_t frames) noexcept {
    std::fill(buffer_.begin(),buffer_.end(),0.f);
    std::fill_n(decisions_.begin(),frames,std::uint8_t{0});std::fill_n(threshold_.begin(),frames,0.f);
    std::size_t index=0;
    for(std::size_t i=1;i<frames;++i){
        buffer_[index]=gated_[i];index=(i+1)%buffer_.size();
        threshold_[i]=threshold();if(!std::isfinite(threshold_[i]))return false;
        decisions_[i]=std::uint8_t(gated_[i]>threshold_[i]);
    }
    return true;
}
void PreparedAdaptiveOnsetSelection::clean(std::size_t frames) noexcept {
    for(std::size_t i=0;i<frames;++i)
        result_[i]=std::uint8_t(decisions_[i] && (!i || !decisions_[i-1]) && (i+1==frames || decisions_[i+1]));
}
bool PreparedAdaptiveOnsetSelection::process(std::span<const float> input,float weights,std::span<std::uint8_t> output) noexcept {
    result_frames_=0;
    if(!maximum_frames_ || input.size()>maximum_frames_ || output.size()!=input.size() || !std::isfinite(weights))return false;
    if(!gate(input,weights) || !decide(input.size()))return false;
    clean(input.size());std::copy_n(result_.begin(),input.size(),output.begin());result_frames_=input.size();return true;
}
bool PreparedAdaptiveOnsetSelection::onset_times(std::span<float> output,std::size_t& count) const noexcept {
    if(!maximum_frames_)return false;
    const auto needed=std::size_t(std::count(result_.begin(),result_.begin()+std::ptrdiff_t(result_frames_),std::uint8_t{1}));
    if(output.size()<needed)return false;
    std::size_t next=0;
    for(std::size_t i=0;i<result_frames_;++i)if(result_[i])output[next++]=float(i)/config_.frame_rate;
    count=next;return true;
}
}
