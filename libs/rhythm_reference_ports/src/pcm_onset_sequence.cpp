// SPDX-License-Identifier: GPL-3.0-or-later
// aubio src/onset/onset.c composition and control/reset semantics at
// ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2006-2013 Paul Brossier; GPL3 or later, no warranty, see ../COPYING.
#include <rhythm_reference_ports/pcm_onset_sequence.h>
#include <rhythm_reference_ports/sample_timing.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace rhythm_reference_ports {
bool PreparedPcmOnsetSequence::prepare(PcmOnsetSequenceConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedPcmOnsetSequence next;std::size_t bytes=0;
    if(!storage_bytes(config,bytes,factory) || !next.configure(config,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmOnsetSequence::configure(PcmOnsetSequenceConfig c,const dsp_kernels::FourierBackendFactory* factory) {
    if(!c.sample_rate || c.sample_rate>INT32_MAX)return false;
    config_=c;
    if(!prepare_components(factory))return false;
    values_.resize(spectrum_.bins());phase_.resize(spectrum_.bins());prepared_=true;
    return apply_default_parameters(c.method);
}
bool PreparedPcmOnsetSequence::prepare_components(const dsp_kernels::FourierBackendFactory* factory) {
    if(!spectrum_.prepare(config_.spectrum,{},factory) || !descriptor_.prepare(spectrum_.bins(),config_.method))return false;
    OnsetMagnitudeConfig magnitude;
    magnitude.whitening.frame_period_s=float(config_.spectrum.hop_samples)/float(config_.sample_rate);
    OnsetDecisionConfig decision;
    return magnitude_.prepare(spectrum_.bins(),magnitude) && peaks_.prepare() &&
        onset_decision_parameters(config_.method,config_.spectrum.hop_samples,float(config_.sample_rate),decision) && decision_.prepare(decision);
}
bool PreparedPcmOnsetSequence::apply_default_parameters(OnsetRecipe method) noexcept {
    OnsetDecisionConfig timing;
    if(!prepared_ || !onset_decision_parameters(method,config_.spectrum.hop_samples,float(config_.sample_rate),timing))return false;
    const auto p=onset_recipe_parameters(method);
    if(!peaks_.set_threshold(p.threshold) || !magnitude_.set_whitening(p.whitening) || !magnitude_.set_compression(p.compression) ||
        !decision_.set_delay_samples(timing.delay_samples) || !decision_.set_minimum_interval_samples(timing.minimum_interval_samples))return false;
    silence_db_=p.silence_db;
    if(method!=OnsetRecipe::SpectralFlux)return true;
    return magnitude_.set_whitening_relax_time(p.whitening_config.relax_time_s) && magnitude_.set_whitening_floor(p.whitening_config.floor);
}
void PreparedPcmOnsetSequence::reset() noexcept {decision_.reset();}
void PreparedPcmOnsetSequence::restart() noexcept {
    spectrum_.restart();descriptor_.restart();magnitude_.reset_whitening();peaks_.reset();decision_.reset();
    std::fill(values_.begin(),values_.end(),0.f);std::fill(phase_.begin(),phase_.end(),0.f);descriptor_value_=0;thresholded_value_=0;
}
bool PreparedPcmOnsetSequence::set_silence(float value) noexcept {
    if(!prepared_ || !std::isfinite(value))return false;
    silence_db_=value;return true;
}
bool PreparedPcmOnsetSequence::set_delay_seconds(float value) noexcept {
    std::uint64_t samples=0;
    return unsigned_sample_offset(value,config_.sample_rate,false,samples) && decision_.set_delay_samples(samples);
}
bool PreparedPcmOnsetSequence::set_minimum_interval_seconds(float value) noexcept {
    std::uint64_t samples=0;
    return unsigned_sample_offset(value,config_.sample_rate,true,samples) && decision_.set_minimum_interval_samples(samples);
}
bool PreparedPcmOnsetSequence::process_hop(std::span<const float> input,PcmOnsetObservation& output) noexcept {
    if(!prepared_ || !spectrum_.process_hop(input,values_,phase_) || !magnitude_.process(values_))return false;
    PcmOnsetObservation frame;
    if(!descriptor_.process(values_,phase_,frame.descriptor) || !peaks_.process(frame.descriptor,frame.peak) ||
        !pcm_is_silent(input,silence_db_,frame.silent) || !decision_.process(frame.peak.position,frame.silent,frame.onset))return false;
    descriptor_value_=frame.descriptor;thresholded_value_=frame.peak.thresholded;output=frame;return true;
}
}
