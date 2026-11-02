// SPDX-License-Identifier: ISC
// librosa beat.py beat_track/plp PCM paths,
// pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/pcm_beat_tracking.h>
#include <algorithm>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
namespace {
struct BeatBatchInputGuard {
    std::span<BeatTrackingChannelInput> inputs;
    ~BeatBatchInputGuard(){std::fill(inputs.begin(),inputs.end(),BeatTrackingChannelInput{});}
};
}
bool PreparedPcmBeatTracking::prepare(std::size_t maximum,PcmBeatTrackingConfig c,std::span<const float> window) {
    std::size_t required=0;if(!storage_bytes(maximum,c,window,required))return false;
    if(!c.maximum_channels || c.onset.spectrum.sample_rate!=c.beats.tempo.sample_rate ||
        c.onset.onset.strength.hop_length!=c.beats.tempo.hop_samples)return false;
    PreparedPcmBeatTracking next;
    if(!next.onset_.prepare(maximum,c.onset,window))return false;
    const auto frames=next.onset_.frames(maximum);
    if(!frames || c.maximum_channels>SIZE_MAX/frames || !next.beats_.prepare(frames,c.beats))return false;
    next.maximum_frames_=frames;next.batch_envelopes_.resize(frames*c.maximum_channels);next.batch_inputs_.resize(c.maximum_channels);
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmBeatTracking::process(std::span<const float> pcm,BeatTrackingSequenceResult& output,
    std::optional<std::span<const float>> bpm,std::span<const float> prior) noexcept {
    return onset_.process(pcm) && beats_.process(onset_.values(),output,bpm,prior);
}
bool PreparedPcmBeatTracking::process_channels(std::span<const PcmBeatTrackingInput> channels,BeatTrackingDestination output) noexcept {
    if(channels.size()>batch_inputs_.size() || !output.write)return false;
    auto inputs=std::span(batch_inputs_).first(channels.size());BeatBatchInputGuard guard{inputs};
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];const auto count=onset_.frames(input.pcm.size());
        if(count>maximum_frames_)return false;
        auto envelope=std::span(batch_envelopes_).subspan(channel*maximum_frames_,count);
        if(!onset_.process_into(input.pcm,envelope))return false;
        inputs[channel]={envelope,input.bpm,input.log_prior};
    }
    return beats_.process_channels(inputs,output);
}
bool PreparedPcmLocalPulse::prepare(std::size_t maximum,PcmLocalPulseConfig c,
    std::span<const float> spectral_window,std::span<const float> temporal_window) {
    std::size_t required=0;if(!storage_bytes(maximum,c,spectral_window,temporal_window,required))return false;
    if(c.onset.spectrum.sample_rate!=c.pulse.sample_rate || c.onset.onset.strength.hop_length!=c.pulse.hop_samples)return false;
    PreparedPcmLocalPulse next;
    if(!next.onset_.prepare(maximum,c.onset,spectral_window) ||
        !next.pulse_.prepare(next.onset_.frames(maximum),c.pulse,temporal_window))return false;
    next.batch_pulse_.resize(next.onset_.frames(maximum));if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmLocalPulse::process(std::span<const float> pcm,std::span<float> output,std::span<const float> prior,FloatMatrixSource prior_rows) noexcept {
    return onset_.process(pcm) && pulse_.process(onset_.values(),output,prior,prior_rows);
}
bool PreparedPcmLocalPulse::process_channels(std::span<const std::span<const float>> channels,
    FloatMatrixBatchDestination output,std::span<const float> prior) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel)
        if(!process_channel(channels[channel],channel,output,prior,{}))return false;
    return true;
}
bool PreparedPcmLocalPulse::process_channel(std::span<const float> pcm,std::size_t channel,
    FloatMatrixBatchDestination output,std::span<const float> prior,FloatMatrixSource prior_rows) noexcept {
    const auto frames=onset_.frames(pcm.size());
    if(frames>batch_pulse_.size() || !process(pcm,std::span(batch_pulse_).first(frames),prior,prior_rows))return false;
    for(std::size_t frame=0;frame<frames;++frame)
        if(!output.write(output.context,channel,frame,std::span(batch_pulse_).subspan(frame,1)))return false;
    return true;
}
bool PreparedPcmLocalPulse::process_channels(std::span<const PcmLocalPulseInput> channels,FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];
        if(!process_channel(input.pcm,channel,output,input.log_prior,input.log_prior_rows))return false;
    }
    return true;
}
}


namespace dsp_kernels {
bool PreparedPcmBeatTracking::storage_bytes(std::size_t maximum,PcmBeatTrackingConfig c,std::span<const float> window,std::size_t& out) noexcept {
    if(!c.maximum_channels || c.onset.spectrum.sample_rate!=c.beats.tempo.sample_rate ||
       c.onset.onset.strength.hop_length!=c.beats.tempo.hop_samples)return false;
    std::size_t bytes=0,nested=0,frames=0,count=0;
    if(!PreparedPcmOnsetEnvelope::geometry(maximum,c.onset,{},frames) || !frames || !prepared_storage::mul_bytes(frames,c.maximum_channels,count) ||
       !PreparedPcmOnsetEnvelope::storage_bytes(maximum,c.onset,window,{},bytes) || !PreparedEnvelopeBeatTracking::storage_bytes(frames,c.beats,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,count,sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.maximum_channels,sizeof(BeatTrackingChannelInput)))return false;
    out=bytes;return true;
}
bool PreparedPcmLocalPulse::storage_bytes(std::size_t maximum,PcmLocalPulseConfig c,std::span<const float> sw,std::span<const float> tw,std::size_t& out) noexcept {
    if(c.onset.spectrum.sample_rate!=c.pulse.sample_rate || c.onset.onset.strength.hop_length!=c.pulse.hop_samples)return false;
    std::size_t bytes=0,nested=0,frames=0;
    if(!PreparedPcmOnsetEnvelope::geometry(maximum,c.onset,{},frames) || !PreparedPcmOnsetEnvelope::storage_bytes(maximum,c.onset,sw,{},bytes) ||
       !PreparedEnvelopeLocalPulse::storage_bytes(frames,c.pulse,tw,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,frames,sizeof(float)))return false;
    out=bytes;return true;
}
}
