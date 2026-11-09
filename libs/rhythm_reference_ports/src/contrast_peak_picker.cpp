// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia SuperFluxPeaks at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/contrast_peak_picker.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <dsp_kernels/prepared_storage.h>

namespace rhythm_reference_ports {
namespace {
bool nonnegative(float value) noexcept {return std::isfinite(value) && value>=0;}
bool filter_length(float rate,float milliseconds,std::size_t& length) noexcept {
    const float value=rate*milliseconds/1000.f;
    const float maximum=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(milliseconds) || milliseconds<=0 || !std::isfinite(value) || value<2 || value>maximum)return false;
    length=std::size_t(value);return true;
}
}
bool PreparedContrastPeakPicker::prepare_filters(ContrastPeakPickerConfig config) {
    std::size_t average=0,maximum=0;
    return filter_length(config.frame_rate,config.pre_average_ms,average) &&
        filter_length(config.frame_rate,config.pre_maximum_ms,maximum) &&
        average_.prepare(average) && maximum_.prepare(maximum,true);
}
bool PreparedContrastPeakPicker::configure(std::size_t capacity,ContrastPeakPickerConfig config) {
    if(!capacity || !std::isfinite(config.frame_rate) || config.frame_rate<=0 ||
       !nonnegative(config.threshold) || !nonnegative(config.ratio_threshold) ||
       !std::isfinite(config.combine_ms) || config.combine_ms<=0)return false;
    if(!prepare_filters(config))return false;
    config_=config;combine_seconds_=config.combine_ms/1000.f;
    averages_.resize(capacity);maxima_.resize(capacity);onsets_.resize(capacity);return true;
}
bool PreparedContrastPeakPicker::prepare(std::size_t capacity,ContrastPeakPickerConfig config) {
    PreparedContrastPeakPicker next;std::size_t bytes=0;
    if(!storage_bytes(capacity,config,bytes) || !next.configure(capacity,config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedContrastPeakPicker::storage_bytes(std::size_t capacity,ContrastPeakPickerConfig config,std::size_t& output) noexcept {
    if(!capacity || !std::isfinite(config.frame_rate) || config.frame_rate<=0 || !nonnegative(config.threshold) ||
       !nonnegative(config.ratio_threshold) || !std::isfinite(config.combine_ms) || config.combine_ms<=0)return false;
    std::size_t average=0,maximum=0,bytes=0,filter=0;
    if(!filter_length(config.frame_rate,config.pre_average_ms,average) || !filter_length(config.frame_rate,config.pre_maximum_ms,maximum) ||
       !PreparedCausalAverage::storage_bytes(average,bytes) || !PreparedBufferedMaximum::storage_bytes(maximum,filter) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,filter) || !dsp_kernels::prepared_storage::add_array(bytes,capacity,3*sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedContrastPeakPicker::workspace_bytes() const noexcept {
    return maximum_.workspace_bytes()+average_.workspace_bytes()+
        (averages_.capacity()+maxima_.capacity()+onsets_.capacity())*sizeof(float);
}
void PreparedContrastPeakPicker::reset(bool clear_counter) noexcept {
    maximum_.reset();average_.reset();start_time_=0;count_=0;processed_=0;
    if(clear_counter)detections_=0;
}
bool PreparedContrastPeakPicker::over_threshold(float value,float average) const noexcept {
    const bool linear=config_.threshold>0 && value>average+config_.threshold;
    const bool ratio=config_.ratio_threshold>0 && average>0 && value/average>config_.ratio_threshold;
    return linear || ratio;
}
bool PreparedContrastPeakPicker::scan(std::span<const float> input) noexcept {
    for(std::size_t i=0;i<input.size();++i){
        const float value=input[i];
        if(value!=maxima_[i] || value<=1e-8f || !over_threshold(value,averages_[i]))continue;
        const float time=start_time_+float(i)/config_.frame_rate;
        if(!std::isfinite(time))return false;
        if(!count_ || time-onsets_[count_-1]>combine_seconds_)onsets_[count_++]=time;
    }
    return true;
}
bool PreparedContrastPeakPicker::finish_scan(std::span<const float> input,float next) noexcept {
    if(!scan(input) || count_>std::numeric_limits<std::uint64_t>::max()-detections_){count_=0;return false;}
    detections_+=count_;start_time_=next;processed_=input.size();
    return true;
}
bool PreparedContrastPeakPicker::process(std::span<const float> input) noexcept {
    count_=0;processed_=0;
    if(onsets_.empty() || input.size()>onsets_.size())return false;
    if(input.empty())return true;
    const float next=start_time_+float(input.size())/config_.frame_rate;
    if(!std::isfinite(next))return false;
    if(!average_.process(input,std::span(averages_).first(input.size())) ||
       !maximum_.process(input,std::span(maxima_).first(input.size())))return false;
    return finish_scan(input,next);
}
std::span<const float> PreparedContrastPeakPicker::averages(std::size_t count) const noexcept {
    return count<=processed_?std::span<const float>(averages_).first(count):std::span<const float>{};
}
std::span<const float> PreparedContrastPeakPicker::maxima(std::size_t count) const noexcept {
    return count<=processed_?std::span<const float>(maxima_).first(count):std::span<const float>{};
}
bool PreparedContrastPeakAccumulator::prepare(std::size_t chunk,std::size_t capacity,ContrastPeakPickerConfig config) {
    std::size_t bytes=0;if(!storage_bytes(chunk,capacity,config,bytes))return false;
    PreparedContrastPeakAccumulator next;
    if(!next.picker_.prepare(chunk,config))return false;
    next.frame_rate_=config.frame_rate;next.combine_seconds_=config.combine_ms/1000.f;next.onsets_.resize(capacity);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedContrastPeakAccumulator::storage_bytes(std::size_t chunk,std::size_t capacity,ContrastPeakPickerConfig config,std::size_t& output) noexcept {
    std::size_t bytes=0;
    if(!capacity || !PreparedContrastPeakPicker::storage_bytes(chunk,config,bytes) ||
       !dsp_kernels::prepared_storage::add_array(bytes,capacity,sizeof(float)))return false;
    output=bytes;return true;
}
void PreparedContrastPeakAccumulator::reset() noexcept {
    current_time_=0;count_=0;picker_.reset();
}
bool PreparedContrastPeakAccumulator::append(std::span<const float> batch) noexcept {
    if(batch.empty())return true;
    const bool trim=count_ && current_time_+batch.front()-onsets_[count_-1]<combine_seconds_;
    const auto retained=trim?(count_>1?batch.size()-1:0):batch.size();
    if(retained>onsets_.size()-count_)return false;
    std::copy_n(batch.begin(),retained,onsets_.begin()+std::ptrdiff_t(count_));count_+=retained;return true;
}
bool PreparedContrastPeakAccumulator::consume(std::span<const float> input) noexcept {
    if(onsets_.empty() || !picker_.process(input))return false;
    const float next=current_time_+float(input.size())/frame_rate_;
    if(!std::isfinite(next) || !append(picker_.onsets()))return false;
    current_time_=next;return true;
}
bool PreparedContrastPeakAccumulator::finish(std::span<float> output,std::size_t& count) noexcept {
    if(output.size()<count_ || onsets_.empty())return false;
    std::copy_n(onsets_.begin(),count_,output.begin());count=count_;reset();return true;
}
}
