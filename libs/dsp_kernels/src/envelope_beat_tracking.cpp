// SPDX-License-Identifier: ISC
// librosa beat.py beat_track, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/envelope_beat_tracking.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
bool PreparedEnvelopeBeatTracking::prepare(std::size_t frames,EnvelopeBeatTrackingConfig c) {
    std::size_t required=0;if(!storage_bytes(frames,c,required))return false;
    PreparedEnvelopeBeatTracking next;
    if(!next.configure(frames,c))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
void PreparedEnvelopeBeatTracking::allocate(std::size_t frames) {
    bpm_.resize(frames);mask_.resize(frames);events_.resize(frames);
    samples_.resize(frames);seconds_.resize(frames);
}
bool PreparedEnvelopeBeatTracking::configure(std::size_t frames,EnvelopeBeatTrackingConfig c) {
    if(!frames || !c.tempo.hop_samples || (c.sparse && unsigned(c.units)>unsigned(EventCoordinateUnit::Seconds)))return false;
    c.tracking.frame_rate=c.tempo.sample_rate/float(c.tempo.hop_samples);
    if(!tempo_.prepare(frames,c.tempo) || !tracker_.prepare(frames,c.tracking))return false;
    config_=c;maximum_frames_=frames;allocate(frames);return true;
}
bool PreparedEnvelopeBeatTracking::select_tempo(std::span<const float> envelope,
    std::optional<std::span<const float>> supplied,std::span<const float> prior) noexcept {
    if(supplied){
        const auto values=*supplied;
        if(values.size()!=1 && values.size()!=envelope.size())return false;
        std::copy(values.begin(),values.end(),bpm_.begin());bpm_count_=values.size();return true;
    }
    bpm_count_=tempo_.output_frames(envelope.size());
    return bpm_count_ && tempo_.process(envelope,std::span(bpm_).first(bpm_count_),prior);
}
bool PreparedEnvelopeBeatTracking::silence(std::span<const float> envelope) noexcept {
    // Clear DP diagnostics through its existing empty-evidence path.
    const float neutral=config_.tracking.frame_rate*30.f;
    if(!tracker_.process(envelope,std::span(&neutral,1),std::span(mask_).first(envelope.size())))return false;
    bpm_[0]=0;bpm_count_=1;return true;
}
bool PreparedEnvelopeBeatTracking::format_events(std::size_t count,BeatTrackingSequenceResult& value) noexcept {
    if(config_.units==EventCoordinateUnit::Frames)value.frames=std::span(events_).first(count);
    else if(config_.units==EventCoordinateUnit::Samples){
        for(std::size_t i=0;i<count;++i)if(!event_sample_position(events_[i],config_.tempo.hop_samples,samples_[i]))return false;
        value.samples=std::span(samples_).first(count);
    } else {
        for(std::size_t i=0;i<count;++i)if(!event_time_position(events_[i],config_.tempo.hop_samples,config_.tempo.sample_rate,seconds_[i]))return false;
        value.seconds=std::span(seconds_).first(count);
    }
    return true;
}
bool PreparedEnvelopeBeatTracking::result(std::size_t frames,BeatTrackingSequenceResult& output) noexcept {
    BeatTrackingSequenceResult value;value.bpm=std::span(bpm_).first(bpm_count_);
    std::size_t count=0;
    if(!config_.sparse)value.dense=std::span(mask_).first(frames);
    else {
        for(std::size_t i=0;i<frames;++i)if(mask_[i])events_[count++]=i;
        if(!format_events(count,value))return false;
    }
    output=value;return true;
}
bool PreparedEnvelopeBeatTracking::process_active(std::span<const float> envelope,BeatTrackingSequenceResult& output,
    std::optional<std::span<const float>> supplied,std::span<const float> prior,bool active) noexcept {
    if(!maximum_frames_ || envelope.size()>maximum_frames_ ||
        !std::all_of(envelope.begin(),envelope.end(),[](float x){return std::isfinite(x);}))return false;
    if(!active){
        if(!silence(envelope))return false;
    } else if(!select_tempo(envelope,supplied,prior) ||
        !tracker_.process(envelope,std::span(bpm_).first(bpm_count_),std::span(mask_).first(envelope.size())))return false;
    return result(envelope.size(),output);
}
bool PreparedEnvelopeBeatTracking::process(std::span<const float> envelope,BeatTrackingSequenceResult& output,
    std::optional<std::span<const float>> bpm,std::span<const float> prior) noexcept {
    const bool active=std::any_of(envelope.begin(),envelope.end(),[](float x){return x!=0;});
    return process_active(envelope,output,bpm,prior,active);
}
bool PreparedEnvelopeBeatTracking::process_channels(std::span<const BeatTrackingChannelInput> channels,BeatTrackingDestination output) noexcept {
    if(!output.write || (config_.sparse && channels.size()>1))return false;
    const bool active=std::any_of(channels.begin(),channels.end(),[](const auto& c){
        return std::any_of(c.envelope.begin(),c.envelope.end(),[](float x){return x!=0;});
    });
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];BeatTrackingSequenceResult result;
        if(!process_active(input.envelope,result,input.bpm,input.log_prior,active) ||
            !output.write(output.context,channel,result))return false;
    }
    return true;
}
}

namespace dsp_kernels {
bool PreparedEnvelopeBeatTracking::storage_bytes(std::size_t n,EnvelopeBeatTrackingConfig c,std::size_t& out) noexcept {
    if(!n || !c.tempo.hop_samples || (c.sparse && unsigned(c.units)>unsigned(EventCoordinateUnit::Seconds)))return false;
    c.tracking.frame_rate=c.tempo.sample_rate/float(c.tempo.hop_samples);
    std::size_t bytes=0,nested=0;
    if(!PreparedEnvelopeTempo::storage_bytes(n,c.tempo,bytes) || !PreparedBeatTracker::storage_bytes(n,c.tracking,nested))return false;
    nested-=sizeof(PreparedBeatTracker);
    if(!prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,n,
        sizeof(float)+sizeof(std::uint8_t)+sizeof(std::size_t)+sizeof(std::uint64_t)+sizeof(double)))return false;
    out=bytes;return true;
}
std::size_t PreparedEnvelopeBeatTracking::workspace_bytes() const noexcept {
    return tempo_.workspace_bytes()+tracker_.workspace_bytes()+bpm_.capacity()*sizeof(float)+mask_.capacity()+
        events_.capacity()*sizeof(std::size_t)+samples_.capacity()*sizeof(std::uint64_t)+seconds_.capacity()*sizeof(double);
}
}
