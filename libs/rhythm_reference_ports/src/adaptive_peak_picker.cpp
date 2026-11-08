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
    PreparedAdaptivePeakPicker next;next.config_=c;next.scratch_.resize(c.pre+c.post+1);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedAdaptivePeakPicker::storage_bytes(AdaptivePeakConfig c,std::size_t& output) noexcept {
    if(c.pre>SIZE_MAX-c.post || c.pre+c.post==SIZE_MAX || !std::isfinite(c.threshold))return false;
    if(c.statistic<ThresholdStatistic::Median || c.statistic>ThresholdStatistic::Mean)return false;
    std::size_t bytes=0;
    if(!dsp_kernels::prepared_storage::add_array(bytes,c.pre+c.post+1,sizeof(float)))return false;
    output=bytes;return true;
}
void PreparedAdaptivePeakPicker::reset() noexcept {peak_.fill(0);}
bool PreparedAdaptivePeakPicker::set_threshold(float threshold) noexcept {
    if(scratch_.empty() || !std::isfinite(threshold))return false;
    config_.threshold=threshold;return true;
}
float PreparedAdaptivePeakPicker::statistic(std::span<const float> values) noexcept {
    std::copy(values.begin(),values.end(),scratch_.begin());
    if(config_.threshold_function)return config_.threshold_function(scratch_);
    if(config_.statistic==ThresholdStatistic::Median)
        return dsp_kernels::median_in_place(scratch_,dsp_kernels::EvenMedian::Lower);
    float value=0;
    return dsp_kernels::arithmetic_mean_float32(scratch_,value)?value:std::numeric_limits<float>::quiet_NaN();
}
bool PreparedAdaptivePeakPicker::process(std::span<const float> values,AdaptivePeakObservation& output) noexcept {
    if(scratch_.empty() || values.size()!=scratch_.size() || !finite(values))return false;
    float average=0;
    if(!dsp_kernels::arithmetic_mean_float32(values,average))return false;
    const float median=statistic(values);
    const float thresholded=values[config_.post]-median-average*config_.threshold;
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
