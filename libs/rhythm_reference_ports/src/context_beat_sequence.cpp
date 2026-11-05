// SPDX-License-Identifier: GPL-3.0-or-later
// Port of aubio/src/tempo/tempo.c at
// aubio/aubio@ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2006-2009 Paul Brossier <piem@aubio.org>.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/context_beat_sequence.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
std::size_t source_window(float rate) noexcept {
    const float requested=5.8f*rate;
    if(!std::isfinite(requested) || requested<0 || requested>=float(UINT32_MAX))return 0;
    const auto truncated=std::uint32_t(requested);std::size_t size=4;
    while(size<truncated){if(size>SIZE_MAX/2)return 0;size*=2;}
    return size;
}
}
bool PreparedContextBeatSequence::prepare(ContextBeatSequenceConfig config) {
    PreparedContextBeatSequence next;std::size_t bytes=0;
    if(!storage_bytes(config,bytes) || !next.configure(config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedContextBeatSequence::resolve_configuration(ContextBeatSequenceConfig config,ContextBeatSequenceConfig& output) noexcept {
    if(!config.hop_samples || config.tatums_per_beat<1 || config.tatums_per_beat>64)return false;
    if(!config.tracker.window)config.tracker.window=source_window(config.tracker.frame_rate);
    std::size_t bytes=0;if(!PreparedContextBeatTracker::storage_bytes(config.tracker,bytes))return false;
    output=config;return true;
}
bool PreparedContextBeatSequence::storage_bytes(ContextBeatSequenceConfig supplied,std::size_t& output) noexcept {
    ContextBeatSequenceConfig config;if(!resolve_configuration(supplied,config))return false;
    std::size_t bytes=0,history=0;
    if(!PreparedContextBeatTracker::storage_bytes(config.tracker,bytes) || !ScalarHistory::storage_bytes(config.tracker.window,history) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,history) ||
       !dsp_kernels::prepared_storage::add_array(bytes,config.tracker.window/4+1,sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedContextBeatSequence::configure(ContextBeatSequenceConfig supplied) {
    ContextBeatSequenceConfig config;if(!resolve_configuration(supplied,config))return false;
    if(!tracker_.prepare(config.tracker) || !history_.prepare(config.tracker.window))return false;
    offsets_.resize(tracker_.maximum_beats());config_=config;reset();return true;
}
void PreparedContextBeatSequence::reset() noexcept {
    tracker_.reset();history_.clear();std::fill(offsets_.begin(),offsets_.end(),0.f);
    offset_count_=0;block_position_=0;total_=0;last_beat_=0;last_tatum_=0;
}
bool PreparedContextBeatSequence::advance_block() noexcept {
    if(block_position_!=std::ptrdiff_t(tracker_.step())-1)return true;
    if(!tracker_.process(history_.values(),offsets_,offset_count_))return false;
    for(std::size_t i=0;i<tracker_.step();++i)history_.push(0);
    block_position_=-1;return true;
}
bool PreparedContextBeatSequence::begin_frame() noexcept {return !offsets_.empty() && advance_block();}
float PreparedContextBeatSequence::scheduled_fraction(bool silent,bool& matched) noexcept {
    float tactus=0;matched=false;
    for(std::size_t i=0;i<offset_count_;++i){
        const float whole=std::floor(offsets_[i]);
        if(float(block_position_)!=whole)continue;
        matched=true;tactus=silent?0:offsets_[i]-whole;
        last_beat_=total_+std::uint64_t(std::round(tactus*float(config_.hop_samples)));
        last_tatum_=last_beat_;
    }
    return tactus;
}
float PreparedContextBeatSequence::period_samples() const noexcept {
    return float(config_.hop_samples)*tracker_.period_frames();
}
std::int64_t PreparedContextBeatSequence::last_beat_sample() const noexcept {
    return std::int64_t(last_beat_)+config_.delay_samples;
}
std::int64_t PreparedContextBeatSequence::last_tatum_sample() const noexcept {
    return std::int64_t(last_tatum_)-config_.delay_samples;
}
bool PreparedContextBeatSequence::process(float descriptor,bool silent,ContextBeatFrame& output) noexcept {
    constexpr auto reserve=std::uint64_t(UINT32_MAX)+std::uint64_t(INT32_MAX)+1;
    if(offsets_.empty() || !std::isfinite(descriptor) || total_>std::uint64_t(INT64_MAX)-reserve)return false;
    if(!advance_block())return false;
    ++block_position_;
    history_.set(config_.tracker.window-tracker_.step()+std::size_t(block_position_),descriptor);
    ContextBeatFrame frame;frame.silent=silent;frame.source_tactus=scheduled_fraction(silent,frame.scheduled);
    frame.period_samples=period_samples();frame.bpm=tracker_.bpm();frame.confidence=tracker_.confidence();
    frame.beat_sample=last_beat_sample();total_+=config_.hop_samples;output=frame;return true;
}
unsigned PreparedContextBeatSequence::poll_tatum() noexcept {
    if(offsets_.empty())return 0;
    const auto distance=total_-last_tatum_;const float period=period_samples();
    if(distance<config_.hop_samples){last_tatum_=last_beat_;return 2;}
    if(float(distance)>period/float(config_.tatums_per_beat)){
        if(float(distance+config_.hop_samples)>period)return 0;
        last_tatum_=total_;return 1;
    }
    return 0;
}
bool PreparedContextBeatSequence::set_delay_samples(std::int32_t delay) noexcept {
    if(offsets_.empty())return false;
    config_.delay_samples=delay;return true;
}
bool PreparedContextBeatSequence::set_tatum_signature(unsigned signature) noexcept {
    if(signature<1 || signature>64)return false;
    config_.tatums_per_beat=signature;return true;
}
}
