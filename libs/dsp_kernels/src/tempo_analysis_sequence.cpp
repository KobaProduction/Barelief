// madmom features/tempo.py TempoEstimationProcessor composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/tempo_analysis_sequence.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/frame_timing.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
namespace {
bool window_recipe(std::span<const float> explicit_kernel,std::size_t length,std::vector<float>& output) {
    if(!explicit_kernel.empty()){
        if(!std::all_of(explicit_kernel.begin(),explicit_kernel.end(),[](float x){return std::isfinite(x);}))return false;
        output.assign(explicit_kernel.begin(),explicit_kernel.end());return true;
    }
    output.resize(length);return symmetric_hamming_window(output);
}
}
bool PreparedTempoAnalysisSequence::prepare(std::size_t maximum,TempoAnalysisSequenceConfig config,
                                           std::span<const float> activation,std::span<const float> histogram,std::span<const float> alpha) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,activation,histogram,alpha,bound))return false;
    PreparedTempoAnalysisSequence next;
    if(!next.configure(maximum,config,activation,histogram,alpha))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedTempoAnalysisSequence::configure(std::size_t maximum,TempoAnalysisSequenceConfig config,
                                             std::span<const float> activation,std::span<const float> histogram,std::span<const float> alpha) {
    if(!maximum || config.histogram.channels!=1)return false;
    config_=config;maximum_frames_=maximum;
    if(!prepare_windows(activation,histogram) || !prepare_histogram(maximum,alpha))return false;
    activations_.resize(std::max(maximum,activation_kernel_.size()));
    smoothed_bins_.resize(std::max(histogram_.offline_bin_count(),histogram_.online_coordinates().size()));
    return true;
}
bool PreparedTempoAnalysisSequence::prepare_windows(std::span<const float> activation,std::span<const float> histogram) {
    std::size_t frames=0;
    const bool state=config_.histogram.method==TempoHistogramEstimator::StateModel;
    const bool disable=state && !config_.smooth_state_model_activations && activation.empty();
    if(!disable && !rounded_frame_count(config_.activation_smoothing_seconds,config_.histogram.frame_rate,frames))return false;
    return window_recipe(activation,frames,activation_kernel_) &&
        window_recipe(histogram,config_.histogram_smoothing,histogram_kernel_);
}
bool PreparedTempoAnalysisSequence::prepare_histogram(std::size_t maximum,std::span<const float> alpha) {
    if(!histogram_.prepare(std::max(maximum,activation_kernel_.size()),config_.histogram,alpha))return false;
    const auto offline=histogram_.offline_coordinates(),online=histogram_.online_coordinates();
    if(offline.size()!=histogram_.offline_bin_count() || histogram_kernel_.size()>std::min(offline.size(),online.size()))return false;
    return offline_peaks_.prepare(offline,config_.histogram.frame_rate,config_.interpolate) &&
        online_peaks_.prepare(online,config_.histogram.frame_rate,config_.interpolate);
}
void PreparedTempoAnalysisSequence::reset() noexcept {histogram_.reset();ready_=false;}
bool PreparedTempoAnalysisSequence::valid_input(std::span<const float> input) const noexcept {
    return maximum_frames_ && input.size()<=maximum_frames_ &&
        std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);});
}
std::span<const HistogramTempo> PreparedTempoAnalysisSequence::candidates() const noexcept {
    if(!ready_)return {};
    return last_online_?online_peaks_.candidates():offline_peaks_.candidates();
}
bool PreparedTempoAnalysisSequence::extract(bool online) noexcept {
    const auto raw=histogram_.bins();
    auto smooth=std::span(smoothed_bins_).first(raw.size());
    if(!convolve_same(raw,histogram_kernel_,smooth))return false;
    last_online_=online;ready_=online?online_peaks_.process(smooth):offline_peaks_.process(smooth);
    return ready_;
}
bool PreparedTempoAnalysisSequence::process(std::span<const float> input,bool reset_state) noexcept {
    return config_.histogram.online?process_online(input,reset_state):process_offline(input);
}
bool PreparedTempoAnalysisSequence::process_offline(std::span<const float> input) noexcept {
    ready_=false;
    if(!valid_input(input))return false;
    const auto count=same_convolution_size(input.size(),activation_kernel_.size());
    auto smoothed=std::span(activations_).first(count);
    if(!convolve_same(input,activation_kernel_,smoothed) || !histogram_.process_offline(smoothed))return false;
    return extract(false);
}
bool PreparedTempoAnalysisSequence::process_online(std::span<const float> input,bool reset_state) noexcept {
    ready_=false;
    if(!valid_input(input) || !histogram_.process_online(input,reset_state))return false;
    return extract(true);
}
}


namespace dsp_kernels {
bool PreparedTempoAnalysisSequence::storage_bytes(std::size_t maximum,TempoAnalysisSequenceConfig c,std::span<const float> activation,
    std::span<const float> histogram,std::span<const float> alpha,std::size_t& out) noexcept {
    if(!maximum || c.histogram.channels!=1)return false;
    std::size_t smoothing=0;
    const bool disable=c.histogram.method==TempoHistogramEstimator::StateModel && !c.smooth_state_model_activations && activation.empty();
    if(!disable && !rounded_frame_count(c.activation_smoothing_seconds,c.histogram.frame_rate,smoothing))return false;
    if(!std::all_of(activation.begin(),activation.end(),[](float v){return std::isfinite(v);}) ||
       !std::all_of(histogram.begin(),histogram.end(),[](float v){return std::isfinite(v);}))return false;
    const auto a=activation.empty()?smoothing:activation.size(),h=histogram.empty()?c.histogram_smoothing:histogram.size();
    IntervalHistogramConfig limits;
    if(!interval_histogram_parameters(c.histogram.frame_rate,c.histogram.minimum_bpm,c.histogram.maximum_bpm,c.histogram.history_seconds,limits))return false;
    const auto bins=limits.maximum_interval-limits.minimum_interval+1;std::size_t bytes=0,nested=0;
    if(h>bins || !PreparedTempoHistogramSequence::storage_bytes(std::max(maximum,a),c.histogram,alpha,bytes) ||
       !PreparedHistogramTempoPeaks::storage_bytes(bins,float(limits.minimum_interval),float(limits.maximum_interval),c.interpolate,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,a,sizeof(float)) || !prepared_storage::add_array(bytes,h,sizeof(float)) ||
       !prepared_storage::add_array(bytes,std::max(maximum,a),sizeof(float)) || !prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedTempoAnalysisSequence::workspace_bytes() const noexcept {
    return histogram_.workspace_bytes()+offline_peaks_.workspace_bytes()+online_peaks_.workspace_bytes()+
        (activation_kernel_.capacity()+histogram_kernel_.capacity()+activations_.capacity()+smoothed_bins_.capacity())*sizeof(float);
}
}
