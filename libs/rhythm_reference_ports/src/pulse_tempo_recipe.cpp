// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia PercivalBpmEstimator at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/pulse_tempo_recipe.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool geometry(PulseTempoRecipeConfig config) noexcept {
    const auto limit=std::size_t(std::numeric_limits<int>::max());
    return config.frame_size>=2 && config.frame_size<=limit && config.audio_hop && config.audio_hop<=limit &&
        config.onset_window>=2 && config.onset_window<=limit && config.onset_hop && config.onset_hop<=limit &&
        config.sample_rate>0 && config.minimum_bpm>0 && config.minimum_bpm<config.maximum_bpm;
}
SpectralWindowConfig pulse_spectral_window(dsp_kernels::NumericalPrecision precision) noexcept {
    SpectralWindowConfig shape;shape.precision=precision;shape.shape=SpectralWindowShape::Hamming;shape.normalized=false;
    return shape;
}
}
bool PreparedPulseTempoRecipe::prepare_spectrum(PulseTempoRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig window;window.length=config.frame_size;window.hop=config.audio_hop;
    window.center=false;window.periodic=false;
    std::vector<float> coefficients(config.frame_size);
    if(coefficients.capacity()>config.frame_size)return false;
    const auto shape=pulse_spectral_window(config.window_precision);
    if(!spectral_window(coefficients,shape) || !spectrum_.prepare(window,coefficients,0,false,factory))return false;
    if(!onset_.prepare(spectrum_.bins(),config.frame_size))return false;
    complex_.resize(2*spectrum_.bins());magnitude_.resize(spectrum_.bins());return true;
}
bool PreparedPulseTempoRecipe::prepare_onset_frames(PulseTempoRecipeConfig config,TempoOctaveClassifier classifier,const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig window;window.length=config.onset_window;window.hop=config.onset_hop;
    window.center=false;window.function=dsp_kernels::WindowFunction::Rectangular;window.tail=dsp_kernels::TemporalTail::CoverEnd;
    const float rate=float(config.sample_rate)/float(config.audio_hop);
    if(!onset_window_.prepare(window) || !history_.prepare(config.onset_window) ||
       !correlation_.prepare(config.onset_window,rate,float(config.minimum_bpm),float(config.maximum_bpm),factory) ||
       !consensus_.prepare(rate,classifier))return false;
    tail_.resize(config.onset_window);return true;
}
bool PreparedPulseTempoRecipe::configure(std::size_t maximum,PulseTempoRecipeConfig config,TempoOctaveClassifier classifier,const dsp_kernels::FourierBackendFactory* factory) {
    if(!maximum || maximum>std::size_t(std::numeric_limits<int>::max()) || !geometry(config))return false;
    if(!prepare_spectrum(config,factory) || !prepare_onset_frames(config,classifier,factory))return false;
    maximum_pcm_=maximum;config_=config;reset();return true;
}
bool PreparedPulseTempoRecipe::prepare(std::size_t maximum,PulseTempoRecipeConfig config,TempoOctaveClassifier classifier,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,classifier,bound,factory))return false;
    PreparedPulseTempoRecipe next;
    if(!next.configure(maximum,config,classifier,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedPulseTempoRecipe::reset() noexcept {
    onset_.reset();consensus_.reset();history_.clear();
    onset_count_=0;analyzed_=0;next_end_=config_.onset_window;result_={};diagnostic_frames_=0;
}
bool PreparedPulseTempoRecipe::analyze(std::span<const float> frame) noexcept {
    float lag=-1;
    if(!correlation_.process(frame,lag) || !std::isfinite(lag) ||
       lag>std::nextafter(float(std::numeric_limits<int>::max()),0.f))return false;
    if(!consensus_.add(std::int32_t(lag)))return false;
    ++analyzed_;return true;
}
bool PreparedPulseTempoRecipe::onset_frame(float value) noexcept {
    history_.push(value);++onset_count_;
    if(onset_count_!=next_end_)return true;
    if(!analyze(history_.values()) || next_end_>SIZE_MAX-config_.onset_hop)return false;
    next_end_+=config_.onset_hop;return true;
}
bool PreparedPulseTempoRecipe::audio_frame(std::span<const float> pcm,std::size_t frame) noexcept {
    if(!spectrum_.process_frame(pcm,frame,complex_) || !dsp_kernels::cartesian_to_polar(complex_,magnitude_,{}))return false;
    PulseOnsetObservation value;
    if(!onset_.process(magnitude_,value) || !onset_frame(value.filtered))return false;
    ++diagnostic_frames_;return true;
}
bool PreparedPulseTempoRecipe::finish_tail() noexcept {
    const auto frames=onset_window_.frames(onset_count_);
    if(frames==analyzed_)return true;
    if(frames!=analyzed_+1 || analyzed_>SIZE_MAX/config_.onset_hop)return false;
    const auto start=analyzed_*config_.onset_hop;
    if(start>=onset_count_ || onset_count_-start>config_.onset_window)return false;
    const auto valid=onset_count_-start;
    return onset_window_.read(history_.values().last(valid),0,tail_) && analyze(tail_);
}
bool PreparedPulseTempoRecipe::process(std::span<const float> pcm) noexcept {
    reset();
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_)return false;
    const auto frames=spectrum_.frames(pcm.size());
    for(std::size_t i=0;i<frames;++i)if(!audio_frame(pcm,i))return false;
    if(!finish_tail())return false;
    return consensus_.finish(result_);
}
}
