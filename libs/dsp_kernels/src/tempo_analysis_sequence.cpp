// madmom features/tempo.py TempoEstimationProcessor composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/tempo_analysis_sequence.h>
#include <dsp_kernels/convolution.h>
#include "reference_convolution_fma.h"
#include <dsp_kernels/frame_timing.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>
namespace dsp_kernels {
namespace {
template<typename Scalar> bool window_recipe(std::span<const float> explicit_kernel,std::size_t length,std::vector<Scalar>& output) {
    if(!explicit_kernel.empty()){
        if(!std::all_of(explicit_kernel.begin(),explicit_kernel.end(),[](float x){return std::isfinite(x);}))return false;
        output.assign(explicit_kernel.begin(),explicit_kernel.end());return true;
    }
    output.resize(length);return symmetric_hamming_window(output);
}
bool window_lengths(TempoAnalysisSequenceConfig c,std::span<const float> activation,std::span<const float> histogram,std::size_t& a,std::size_t& h) noexcept {
    std::size_t smoothing=0;
    const bool disable=c.histogram.method==TempoHistogramEstimator::StateModel && !c.smooth_state_model_activations && activation.empty();
    if(!disable && !rounded_frame_count(c.activation_smoothing_seconds,c.histogram.frame_rate,smoothing))return false;
    if(!std::all_of(activation.begin(),activation.end(),[](float v){return std::isfinite(v);}) ||
       !std::all_of(histogram.begin(),histogram.end(),[](float v){return std::isfinite(v);}))return false;
    a=activation.empty()?smoothing:activation.size();h=histogram.empty()?c.histogram_smoothing:histogram.size();return true;
}

bool analysis_admission(std::size_t maximum,TempoAnalysisSequenceConfig c,std::span<const float> activation,std::span<const float> histogram,std::size_t& a,std::size_t& h,IntervalHistogramConfig& limits) noexcept {
    if(!maximum || c.histogram.channels!=1)return false;
    if(!window_lengths(c,activation,histogram,a,h))return false;
    return interval_histogram_parameters(c.histogram.frame_rate,c.histogram.minimum_bpm,c.histogram.maximum_bpm,c.histogram.history_seconds,limits);
}
bool portable_analysis_storage(std::size_t maximum,TempoAnalysisSequenceConfig c,std::size_t a,std::size_t h,const IntervalHistogramConfig& limits,std::span<const float> alpha,std::size_t& out) noexcept {
    const auto bins=limits.maximum_interval-limits.minimum_interval+1;std::size_t bytes=0,nested=0;
    if(h>bins || !PreparedTempoHistogramSequence::storage_bytes(std::max(maximum,a),c.histogram,alpha,bytes) ||
       !PreparedHistogramTempoPeaks::storage_bytes(bins,float(limits.minimum_interval),float(limits.maximum_interval),c.interpolate,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,a,sizeof(float)) || !prepared_storage::add_array(bytes,h,sizeof(float)) ||
       !prepared_storage::add_array(bytes,std::max(maximum,a),sizeof(float)) || !prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    out=bytes;return true;
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
    if(reference())return configure_reference(maximum,activation,histogram,alpha);
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
void PreparedTempoAnalysisSequence::reset() noexcept {histogram_.reset();reference_histogram_.reset();ready_=false;}
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
    if(reference())return process_reference(input);
    const auto count=same_convolution_size(input.size(),activation_kernel_.size());
    auto smoothed=std::span(activations_).first(count);
    if(!convolve_same(input,activation_kernel_,smoothed) || !histogram_.process_offline(smoothed))return false;
    return extract(false);
}
bool PreparedTempoAnalysisSequence::process_online(std::span<const float> input,bool reset_state) noexcept {
    ready_=false;
    if(reference())return false;
    if(!valid_input(input) || !histogram_.process_online(input,reset_state))return false;
    return extract(true);
}
}


namespace dsp_kernels {
bool PreparedTempoAnalysisSequence::storage_bytes(std::size_t maximum,TempoAnalysisSequenceConfig c,std::span<const float> activation,
    std::span<const float> histogram,std::span<const float> alpha,std::size_t& out) noexcept {
    std::size_t a=0,h=0;IntervalHistogramConfig limits;
    if(!analysis_admission(maximum,c,activation,histogram,a,h,limits))return false;
    if(c.precision==NumericalPrecision::DesktopReference){
        if((activation.empty() && a==1) || (histogram.empty() && h==1))return false;
        return reference_storage(maximum,c,a,h,alpha,out);
    }
    if(c.precision!=NumericalPrecision::PortableFloat32)return false;
    return portable_analysis_storage(maximum,c,a,h,limits,alpha,out);
}
std::size_t PreparedTempoAnalysisSequence::workspace_bytes() const noexcept {
    return reference_histogram_.workspace_bytes()+(reference_activation_kernel_.capacity()+reference_histogram_kernel_.capacity()+reference_input_.capacity()+reference_activations_.capacity()+reference_smoothed_.capacity())*sizeof(double)+
        (reference_coordinates_.capacity()+reference_raw_.capacity())*sizeof(float)+histogram_.workspace_bytes()+offline_peaks_.workspace_bytes()+online_peaks_.workspace_bytes()+
        (activation_kernel_.capacity()+histogram_kernel_.capacity()+activations_.capacity()+smoothed_bins_.capacity())*sizeof(float);
}
bool PreparedTempoAnalysisSequence::reference_storage(std::size_t maximum,TempoAnalysisSequenceConfig c,std::size_t a,std::size_t h,std::span<const float> alpha,std::size_t& out) noexcept {
    if(!detail::select_reference_convolution_fma() || c.histogram.online || c.histogram.method!=TempoHistogramEstimator::Comb || c.interpolate)return false;
    IntervalHistogramConfig limits;limits.precision=NumericalPrecision::DesktopReference;limits.alpha=c.histogram.alpha;
    if(!interval_histogram_parameters(c.histogram.frame_rate,c.histogram.minimum_bpm,c.histogram.maximum_bpm,c.histogram.history_seconds,limits))return false;
    const auto bins=limits.maximum_interval-limits.minimum_interval+1;std::size_t bytes=0,nested=0;
    if(h>bins || !PreparedIntervalHistogram::storage_bytes(std::max(maximum,a),limits,alpha,bytes) ||
       !PreparedHistogramTempoPeaks::storage_bytes(bins,float(limits.minimum_interval),float(limits.maximum_interval),false,c.precision,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,a,sizeof(double)) ||
       !prepared_storage::add_array(bytes,h,sizeof(double)) || !prepared_storage::add_array(bytes,maximum,sizeof(double)) ||
       !prepared_storage::add_array(bytes,std::max(maximum,a),sizeof(double)) || !prepared_storage::add_array(bytes,bins,sizeof(double)+3*sizeof(float)))return false;
    out=bytes;return true;
}
bool PreparedTempoAnalysisSequence::configure_reference(std::size_t maximum,std::span<const float> activation,std::span<const float> histogram,std::span<const float> alpha) {
    std::size_t frames=0;IntervalHistogramConfig limits;limits.precision=config_.precision;limits.alpha=config_.histogram.alpha;
    if(!rounded_frame_count(config_.activation_smoothing_seconds,config_.histogram.frame_rate,frames) ||
       !window_recipe(activation,frames,reference_activation_kernel_) || !window_recipe(histogram,config_.histogram_smoothing,reference_histogram_kernel_) ||
       !interval_histogram_parameters(config_.histogram.frame_rate,config_.histogram.minimum_bpm,config_.histogram.maximum_bpm,config_.histogram.history_seconds,limits) ||
       !reference_histogram_.prepare(std::max(maximum,reference_activation_kernel_.size()),limits,alpha))return false;
    const auto axis=reference_histogram_.intervals();reference_coordinates_.assign(axis.begin(),axis.end());
    if(!offline_peaks_.prepare(reference_coordinates_,config_.histogram.frame_rate,false,config_.precision))return false;
    reference_input_.resize(maximum);reference_activations_.resize(std::max(maximum,reference_activation_kernel_.size()));
    reference_smoothed_.resize(axis.size());reference_raw_.resize(axis.size());smoothed_bins_.resize(axis.size());return true;
}
bool PreparedTempoAnalysisSequence::process_reference(std::span<const float> input) noexcept {
    std::copy(input.begin(),input.end(),reference_input_.begin());
    auto smoothed=std::span(reference_activations_).first(same_convolution_size(input.size(),reference_activation_kernel_.size()));
    if(!convolve_same(std::span<const double>(reference_input_).first(input.size()),reference_activation_kernel_,smoothed) ||
       !reference_histogram_.process_offline(smoothed))return false;
    const auto raw=reference_histogram_.reference_bins();
    if(!convolve_same(raw,reference_histogram_kernel_,reference_smoothed_) || !offline_peaks_.process(reference_smoothed_) ||
       !std::all_of(raw.begin(),raw.end(),[](double x){return std::isfinite(x) && std::abs(x)<=double(std::numeric_limits<float>::max());}))return false;
    std::transform(raw.begin(),raw.end(),reference_raw_.begin(),[](double x){return float(x);});
    std::transform(reference_smoothed_.begin(),reference_smoothed_.end(),smoothed_bins_.begin(),[](double x){return float(x);});
    last_online_=false;ready_=true;return true;
}

}
