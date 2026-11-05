// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Aubio src/onset/peakpicker.c and src/temporal/filter.c,
// revision ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2009 Paul Brossier.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version3 or later, without warranty. See ../COPYING.
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <dsp_kernels/order_statistics.h>
#include <dsp_kernels/sequence_statistics.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace rhythm_reference_ports {
namespace {
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
bool peak(std::span<const float> values,std::size_t center) noexcept {
    return values[center]>values[center-1] && values[center]>values[center+1] && values[center]>0;
}
}
bool PreparedAdaptivePeakPicker::prepare(AdaptivePeakConfig c) {
    std::size_t bytes=0;if(!storage_bytes(c,bytes))return false;
    PreparedAdaptivePeakPicker next;next.config_=c;next.history_.resize(c.pre+c.post+1);
    next.filtered_.resize(next.history_.size());next.scratch_.resize(next.history_.size());
    if(!next.prepare_filter(c) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedAdaptivePeakPicker::storage_bytes(AdaptivePeakConfig c,std::size_t& output) noexcept {
    if(c.pre>SIZE_MAX-c.post || c.pre+c.post==SIZE_MAX || !std::isfinite(c.threshold))return false;
    if(!finite(c.feedforward) || !finite(c.feedback) || c.feedback[0]!=1 ||
       c.statistic<ThresholdStatistic::Median || c.statistic>ThresholdStatistic::Mean)return false;
    dsp_kernels::RecursiveFilterOptions options;options.form=dsp_kernels::RecursiveFilterForm::DirectI;
    std::size_t bytes=0;
    if(!dsp_kernels::PreparedRecursiveFilter::storage_bytes(3,3,options,bytes) ||
       !dsp_kernels::prepared_storage::add_array(bytes,c.pre+c.post+1,3*sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedAdaptivePeakPicker::prepare_filter(const AdaptivePeakConfig& c) {
    dsp_kernels::RecursiveFilterOptions filter;filter.form=dsp_kernels::RecursiveFilterForm::DirectI;
    // Float32 ceiling of the source's double 2.e-42 cutoff: identical set of
    // Float32 inputs satisfies abs(x)<cutoff, without double on the hot path.
    filter.input_zero_threshold=std::nextafter(2e-42f,std::numeric_limits<float>::infinity());
    return filter_.prepare(c.feedforward,c.feedback,dsp_kernels::SubnormalStatePolicy::Preserve,filter);
}
void PreparedAdaptivePeakPicker::reset() noexcept {std::fill(history_.begin(),history_.end(),0.f);peak_.fill(0);filter_.reset();}
bool PreparedAdaptivePeakPicker::set_threshold(float threshold) noexcept {
    if(history_.empty() || !std::isfinite(threshold))return false;
    config_.threshold=threshold;return true;
}
bool PreparedAdaptivePeakPicker::filter(std::span<float> values) noexcept {
    filter_.reset();
    const bool ok=filter_.process(values,values);
    filter_.reset();return ok;
}
bool PreparedAdaptivePeakPicker::smooth() noexcept {
    std::copy(history_.begin(),history_.end(),filtered_.begin());if(!filter(filtered_))return false;
    std::reverse_copy(filtered_.begin(),filtered_.end(),scratch_.begin());if(!filter(scratch_))return false;
    std::reverse_copy(scratch_.begin(),scratch_.end(),filtered_.begin());return true;
}
float PreparedAdaptivePeakPicker::statistic() noexcept {
    std::copy(filtered_.begin(),filtered_.end(),scratch_.begin());
    if(config_.threshold_function)return config_.threshold_function(scratch_);
    if(config_.statistic==ThresholdStatistic::Median)
        return dsp_kernels::median_in_place(scratch_,dsp_kernels::EvenMedian::Lower);
    float value=0;
    return dsp_kernels::arithmetic_mean_float32(scratch_,value)?value:std::numeric_limits<float>::quiet_NaN();
}
bool PreparedAdaptivePeakPicker::process(float value,AdaptivePeakObservation& output) noexcept {
    if(history_.empty() || !std::isfinite(value))return false;
    std::move(history_.begin()+1,history_.end(),history_.begin());history_.back()=value;
    if(!smooth())return false;
    float average=0;
    if(!dsp_kernels::arithmetic_mean_float32(filtered_,average))return false;
    const float median=statistic();
    const float thresholded=filtered_[config_.post]-median-average*config_.threshold;
    if(!std::isfinite(thresholded))return false;
    publish(average,median,thresholded,output);return true;
}
void PreparedAdaptivePeakPicker::publish(float average,float median,float thresholded,AdaptivePeakObservation& output) noexcept {
    peak_[0]=peak_[1];peak_[1]=peak_[2];peak_[2]=thresholded;
    const bool picked=config_.peak_function?config_.peak_function(peak_,1):peak(peak_,1);
    const float denominator=peak_[0]-2.f*peak_[1]+peak_[2];
    const float position=picked?1.f+(denominator!=0?.5f*(peak_[0]-peak_[2])/denominator:0):0;
    output={picked,position,thresholded,average,median};
}
}
