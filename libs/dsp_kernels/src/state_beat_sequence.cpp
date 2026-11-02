// madmom features/beats.py DBNBeatTrackingProcessor event/time composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/state_beat_sequence.h>
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
#include <utility>
namespace dsp_kernels {
bool PreparedStateBeatSequence::prepare(std::size_t frames,BeatStateDecoderConfig config,bool online) {
    std::size_t bound=0;if(!storage_bytes(frames,config,bound))return false;
    PreparedStateBeatSequence next;
    if(!next.decoder_.prepare(frames,config))return false;
    next.maximum_frames_=frames;next.frame_rate_=config.frame_rate;next.online_=online;
    next.flags_.resize(frames);next.events_.resize(frames);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedStateBeatSequence::reset() noexcept {decoder_.reset();counter_=0;count_=0;}
bool PreparedStateBeatSequence::publish(std::size_t frames,std::uint64_t first) noexcept {
    for(std::size_t i=0;i<frames;++i){
        if(!flags_[i])continue;
        const float seconds=float(first+i)/frame_rate_;
        if(!std::isfinite(seconds)){count_=0;return false;}
        events_[count_++]=seconds;
    }
    return true;
}
bool PreparedStateBeatSequence::process(std::span<const float> input,bool reset_state) noexcept {
    return online_?process_online(input,reset_state):process_offline(input);
}
bool PreparedStateBeatSequence::process_offline(std::span<const float> input) noexcept {
    count_=0;
    if(!maximum_frames_ || input.size()>maximum_frames_ || !decoder_.offline(input,std::span(flags_).first(input.size())))return false;
    return publish(input.size(),0);
}
bool PreparedStateBeatSequence::process_online(std::span<const float> input,bool reset_state) noexcept {
    count_=0;
    if(!maximum_frames_ || input.size()>maximum_frames_)return false;
    if(reset_state)reset();
    if(input.size()>UINT64_MAX-counter_ || !decoder_.online(input,std::span(flags_).first(input.size()),false))return false;
    const auto first=counter_;counter_+=input.size();return publish(input.size(),first);
}
}

namespace dsp_kernels {
bool PreparedStateBeatSequence::storage_bytes(std::size_t frames,BeatStateDecoderConfig c,std::size_t& out) noexcept {
    std::size_t bytes=0;
    if(!PreparedBeatStateDecoder::storage_bytes(frames,c,bytes) || !prepared_storage::add_array(bytes,frames,sizeof(std::uint8_t)+sizeof(float)))return false;
    out=bytes;return true;
}
}
