// SPDX-License-Identifier: GPL-3.0-or-later
// aubio src/tempo/tempo.c composition at ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2006-2009 Paul Brossier; GPL3 or later, no warranty, see ../COPYING.
#include <rhythm_reference_ports/pcm_context_tempo.h>
#include <rhythm_reference_ports/sample_timing.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace rhythm_reference_ports {
bool PreparedPcmContextTempo::prepare(PcmContextTempoConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedPcmContextTempo next;std::size_t bytes=0;
    if(!storage_bytes(config,bytes,factory) || !next.configure(config,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmContextTempo::configure(PcmContextTempoConfig c,const dsp_kernels::FourierBackendFactory* factory) {
    if(!c.sample_rate || c.sample_rate>INT32_MAX || !std::isfinite(c.silence_db))return false;
    config_=c;
    if(!spectrum_.prepare(c.spectrum,{},factory) || !descriptor_.prepare(spectrum_.bins(),c.method))return false;
    AdaptivePeakConfig peak;peak.threshold=c.threshold;
    auto sequence=c.sequence;sequence.hop_samples=c.spectrum.hop_samples;
    sequence.tracker.frame_rate=float(c.sample_rate)/float(c.spectrum.hop_samples);
    if(!peaks_.prepare(peak) || !sequence_.prepare(sequence))return false;
    values_.resize(spectrum_.bins());phase_.resize(spectrum_.bins());prepared_=true;return true;
}
void PreparedPcmContextTempo::restart() noexcept {
    spectrum_.restart();descriptor_.restart();peaks_.reset();sequence_.reset();
    std::fill(values_.begin(),values_.end(),0.f);std::fill(phase_.begin(),phase_.end(),0.f);
}
bool PreparedPcmContextTempo::set_silence(float value) noexcept {
    if(!prepared_ || !std::isfinite(value))return false;
    config_.silence_db=value;return true;
}
bool PreparedPcmContextTempo::set_delay_seconds(float value) noexcept {
    std::int32_t samples=0;
    return signed_sample_offset(value,config_.sample_rate,samples) && sequence_.set_delay_samples(samples);
}
bool PreparedPcmContextTempo::process_hop(std::span<const float> input,PcmContextTempoObservation& output) noexcept {
    if(!prepared_ || !spectrum_.process_hop(input,values_,phase_))return false;
    PcmContextTempoObservation frame;bool silent=false;
    if(!descriptor_.process(values_,phase_,frame.descriptor) || !sequence_.begin_frame() ||
        !peaks_.process(frame.descriptor,frame.onset_peak) || !pcm_is_silent(input,config_.silence_db,silent) ||
        !sequence_.process(frame.onset_peak.thresholded,silent,frame.beat))return false;
    output=frame;return true;
}
}
