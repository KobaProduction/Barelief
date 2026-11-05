// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogram at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/novelty_tempo_histogram.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
void publish_histogram_frame(std::size_t length,float hop,std::size_t factor,dsp_kernels::TemporalWindowConfig& frame,std::size_t& padding) noexcept {
    frame.length=length;frame.hop=std::size_t(hop);frame.center=true;frame.periodic=false;
    padding=factor*length;
}
}
bool PreparedNoveltyTempoHistogram::geometry(NoveltyTempoHistogramConfig config,dsp_kernels::TemporalWindowConfig& frame,std::size_t& padding) noexcept {
    if(!std::isfinite(config.frame_rate) || config.frame_rate<1 || !std::isfinite(config.frame_seconds) ||
       config.frame_seconds<1 || !std::isfinite(config.overlap) || config.overlap<=0)return false;
    const float requested=config.frame_seconds*config.frame_rate;
    const float bound=std::nextafter(float(std::numeric_limits<int>::max()),0.f);
    if(!std::isfinite(requested) || requested<2 || requested>bound)return false;
    const auto length=std::bit_ceil(std::size_t(requested));
    const float hop=float(length)/config.overlap;
    if(length>std::size_t(std::numeric_limits<int>::max()) || !std::isfinite(hop) || hop<1 || hop>bound)return false;
    if(config.zero_padding_factor>(std::size_t(std::numeric_limits<int>::max())-length)/length)return false;
    publish_histogram_frame(length,hop,config.zero_padding_factor,frame,padding);return true;
}
bool PreparedNoveltyTempoHistogram::windows(std::size_t length,NoveltyTempoHistogramConfig config,std::span<const float> supplied) {
    if(!supplied.empty() && supplied.size()!=length)return false;
    analysis_window_.resize(length);synthesis_window_.resize(length);
    if(supplied.empty()){
        SpectralWindowConfig window;window.shape=config.window;window.precision=config.window_precision;
        if(!spectral_window(analysis_window_,window))return false;
    }else{
        std::copy(supplied.begin(),supplied.end(),analysis_window_.begin());
        if(!normalize_spectral_window(analysis_window_))return false;
    }
    std::copy(analysis_window_.begin(),analysis_window_.end(),synthesis_window_.begin());
    return dsp_kernels::normalize_vector(synthesis_window_,dsp_kernels::VectorNormalization::Maximum);
}
bool PreparedNoveltyTempoHistogram::prepare_histogram(std::size_t maximum,NoveltyTempoHistogramConfig config) {
    FourierTempoHistogramConfig histogram;
    histogram.maximum_peaks=config.maximum_peaks;histogram.weight_by_magnitude=config.weight_by_magnitude;
    auto& path=histogram.path;path.frame_rate=config.frame_rate;path.frequency_step=config.frame_rate/float(fft_size_);
    path.hop=spectrum_.config().hop;path.minimum_bpm=config.minimum_bpm;path.maximum_bpm=config.maximum_bpm;
    path.minimum_change_seconds=config.minimum_change_seconds;path.induced_bpm=config.induced_bpm;
    path.constant_tempo=config.constant_tempo;
    return histogram_.prepare(spectrum_.frames(maximum),spectrum_.bins(),synthesis_window_,histogram);
}
bool PreparedNoveltyTempoHistogram::configure(std::size_t maximum,NoveltyTempoHistogramConfig config,std::span<const float> supplied,const dsp_kernels::FourierBackendFactory* factory) {
    if(!maximum || maximum>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/2)return false;
    dsp_kernels::TemporalWindowConfig frame;std::size_t padding=0;
    if(!geometry(config,frame,padding) || !windows(frame.length,config,supplied))return false;
    if(!spectrum_.prepare(frame,analysis_window_,padding,true,factory))return false;
    fft_size_=frame.length+padding;maximum_samples_=maximum;complex_.resize(2*spectrum_.bins());
    return prepare_histogram(maximum,config);
}
bool PreparedNoveltyTempoHistogram::prepare(std::size_t maximum,NoveltyTempoHistogramConfig config,std::span<const float> supplied,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,supplied,bound,factory))return false;
    PreparedNoveltyTempoHistogram next;
    if(!next.configure(maximum,config,supplied,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedNoveltyTempoHistogram::read_magnitude(const void* context,std::size_t index,std::span<float> output) noexcept {
    const auto& view=*static_cast<const BorrowedInput*>(context);
    auto& self=*view.self;
    return self.spectrum_.process_frame(view.values,index,self.complex_) &&
        dsp_kernels::cartesian_to_polar(self.complex_,output,{});
}
bool PreparedNoveltyTempoHistogram::read_phase(const void* context,std::size_t index,std::span<float> output) noexcept {
    const auto& view=*static_cast<const BorrowedInput*>(context);
    auto& self=*view.self;
    return self.spectrum_.process_frame(view.values,index,self.complex_) &&
        dsp_kernels::cartesian_to_polar(self.complex_,{},output);
}
bool PreparedNoveltyTempoHistogram::process(std::span<const float> novelty) noexcept {
    if(!maximum_samples_ || novelty.size()>maximum_samples_)return false;
    BorrowedInput input{this,novelty};
    return histogram_.process({&input,read_magnitude},{&input,read_phase},spectrum_.frames(novelty.size()));
}
}
