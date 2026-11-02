// SPDX-License-Identifier: ISC
// librosa onset.py onset_detect PCM path, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full license: packaging/licenses/librosa.txt.
#include <dsp_kernels/pcm_onset_events.h>
#include <utility>
namespace dsp_kernels {
bool PreparedPcmOnsetEvents::prepare(std::size_t maximum,PcmOnsetEventsConfig c,std::span<const float> window) {
    if(c.strength.spectrum.sample_rate!=c.detection.sample_rate ||
        c.strength.onset.strength.hop_length!=c.detection.hop_samples)return false;
    PreparedPcmOnsetEvents next;
    if(!next.strength_.prepare(maximum,c.strength,window) || next.strength_.output_features()!=1)return false;
    const auto count=next.strength_.frames(maximum);
    if(!next.detection_.prepare(count,c.detection))return false;
    next.envelope_.resize(count);next.sparse_=c.detection.sparse;*this=std::move(next);return true;
}
bool PreparedPcmOnsetEvents::write_envelope(void* context,std::size_t time,std::span<const float> values) noexcept {
    auto& owner=*static_cast<PreparedPcmOnsetEvents*>(context);
    if(time>=owner.envelope_.size() || values.size()!=1)return false;
    owner.envelope_[time]=values[0];return true;
}
bool PreparedPcmOnsetEvents::process(std::span<const float> pcm,OnsetDetectionResult& output,
    std::optional<std::span<const float>> energy) noexcept {
    frames_=0;
    if(!strength_.process(pcm,{this,write_envelope}))return false;
    const auto count=strength_.frames(pcm.size());
    if(!detection_.process(std::span(envelope_).first(count),output,energy))return false;
    frames_=count;return true;
}
bool PreparedPcmOnsetEvents::process_channels(std::span<const std::span<const float>> channels,OnsetDetectionDestination output) noexcept {
    if(!output.write || (sparse_ && channels.size()>1))return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        OnsetDetectionResult result;
        if(!process(channels[channel],result) || !output.write(output.context,channel,result))return false;
    }
    return true;
}
}
