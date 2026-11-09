// Port of madmom.features.onsets.peak_picking, audio.signal.smooth and
// utils.combine_events at CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception,
// Johannes Kepler University Linz and Austrian Research Institute for
// Artificial Intelligence (OFAI). BSD-style license, full notice:
// packaging/licenses/madmom.txt. Float32 C++ decomposition (c) 2026 Koba Production.
#include <dsp_kernels/activation_peaks.h>
#include <dsp_kernels/convolution.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace dsp_kernels {
namespace {
bool valid_window(std::size_t pre,std::size_t post) noexcept {
    return pre<std::size_t(PTRDIFF_MAX)/4 && post<std::size_t(PTRDIFF_MAX)/4;
}
bool finite(std::span<const float> input) noexcept {
    return std::all_of(input.begin(),input.end(),[](float value){return std::isfinite(value);});
}
bool ordered(std::span<const float> events) noexcept {
    for(std::size_t i=0;i<events.size();++i)
        if(!std::isfinite(events[i]) || (i && events[i]<events[i-1]))return false;
    return true;
}
float combine_pair(float left,float right,EventCombination method) noexcept {
    if(method==EventCombination::Mean)return .5f*left+.5f*right;
    return method==EventCombination::Right?right:left;
}
}
bool PreparedActivationPeaks::storage_bytes(std::size_t frames,ActivationPeakConfig c,std::span<const float> kernel,std::size_t& output) noexcept {
    if(!frames || frames>std::size_t(PTRDIFF_MAX)/4 || kernel.size()>std::size_t(PTRDIFF_MAX)/4 || !finite(kernel))return false;
    if(!std::isfinite(c.threshold) || unsigned(c.smoothing_crop)>unsigned(SameConvolutionCrop::Input) ||
       !valid_window(c.pre_average,c.post_average) || !valid_window(c.pre_maximum,c.post_maximum))return false;
    const auto count=same_convolution_size(frames,kernel.size(),c.smoothing_crop);
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,kernel.size(),sizeof(float)) ||
       !prepared_storage::add_array(bytes,count,2*sizeof(float)+sizeof(std::uint8_t)))return false;
    output=bytes;return true;
}
bool PreparedActivationPeaks::prepare(std::size_t frames,ActivationPeakConfig c,std::span<const float> kernel) {
    std::size_t bytes=0;if(!storage_bytes(frames,c,kernel,bytes))return false;
    PreparedActivationPeaks next;next.maximum_frames_=frames;next.config_=c;next.kernel_.assign(kernel.begin(),kernel.end());
    const auto count=same_convolution_size(frames,kernel.size(),c.smoothing_crop);
    next.smoothed_.resize(count);next.detected_.resize(count);next.result_.resize(count);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
std::size_t PreparedActivationPeaks::output_frames(std::size_t frames) const noexcept {
    if(!maximum_frames_ || frames>maximum_frames_)return 0;
    return same_convolution_size(frames,kernel_.size(),config_.smoothing_crop);
}
bool PreparedActivationPeaks::smooth(std::span<const float> input,std::size_t count) noexcept {
    return convolve_same(input,kernel_,std::span(smoothed_).first(count),config_.smoothing_crop);
}
float PreparedActivationPeaks::mean(std::size_t frame,std::size_t count) const noexcept {
    const auto width=config_.pre_average+config_.post_average+1;
    if(width==1)return 0;
    const auto start=frame>config_.pre_average?frame-config_.pre_average:0;
    const auto stop=std::min(count,frame+config_.post_average+1);float value=0;
    for(auto i=start;i<stop;++i)value+=smoothed_[i];
    return value/float(width);
}
float PreparedActivationPeaks::maximum(std::size_t frame,std::size_t count) const noexcept {
    const auto start=frame>config_.pre_maximum?frame-config_.pre_maximum:0;
    const auto stop=std::min(count,frame+config_.post_maximum+1);
    float value=frame<config_.pre_maximum || frame+config_.post_maximum>=count?0:detected_[start];
    for(auto i=start;i<stop;++i)value=std::max(value,detected_[i]);
    return value;
}
void PreparedActivationPeaks::detect(std::size_t count) noexcept {
    for(std::size_t i=0;i<count;++i)detected_[i]=smoothed_[i]>=mean(i,count)+config_.threshold?smoothed_[i]:0;
    for(std::size_t i=0;i<count;++i)result_[i]=std::uint8_t(detected_[i]!=0 && detected_[i]==maximum(i,count));
}

bool PreparedActivationPeaks::process(std::span<const float> input,std::span<std::uint8_t> output) noexcept {
    const auto count=output_frames(input.size());
    if(!maximum_frames_ || input.size()>maximum_frames_ || output.size()!=count || !finite(input))return false;
    if(input.empty())return kernel_.empty();
    if(!smooth(input,count))return false;
    detect(count);
    std::copy_n(result_.begin(),count,output.begin());return true;
}
namespace {
std::size_t combine_valid_events(std::span<const float> events,float delta,EventCombination method,std::span<float> output) noexcept {
    std::size_t index=0;
    float left=events[0];
    output[0]=left;
    delta+=1e-12f;
    for(std::size_t i=1;i<events.size();++i){
        const float right=events[i];
        if(right-left<=delta)left=combine_pair(left,right,method);
        else{++index;left=right;}
        output[index]=left;
    }
    return index+1;
}
}

bool combine_event_times(std::span<const float> events,float delta,EventCombination method,std::span<float> output,std::size_t& count,EventOrdering order) noexcept {
    if(output.size()<events.size() || !std::isfinite(delta) || delta<0 || unsigned(method)>unsigned(EventCombination::Right))return false;
    if(unsigned(order)>unsigned(EventOrdering::Sequence))return false;
    if(order==EventOrdering::Nondecreasing?!ordered(events):!finite(events))return false;
    if(events.empty()){count=0;return true;}
    count=combine_valid_events(events,delta,method,output);
    return true;
}
}
