// SPDX-License-Identifier: ISC
// librosa onset.py onset_detect, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full license: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/onset_detection_sequence.h>
#include <algorithm>
#include <utility>
namespace dsp_kernels {
namespace {
bool finite_normalized_envelope(std::span<const float> values,bool normalize) noexcept {
    if(!std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    if(!normalize || values.empty())return true;
    const auto [low,high]=std::minmax_element(values.begin(),values.end());
    return std::isfinite(*high-*low);
}
bool envelope_activity(std::span<const float> values,bool normalize) noexcept {
    if(values.empty())return false;
    if(!normalize)return std::any_of(values.begin(),values.end(),[](float x){return x!=0;});
    const auto [low,high]=std::minmax_element(values.begin(),values.end());
    return *low!=*high;
}
}
bool PreparedOnsetDetectionSequence::prepare(std::size_t frames,OnsetDetectionSequenceConfig c,std::size_t energy_frames) {
    std::size_t required=0;if(!storage_bytes(frames,c,energy_frames,required))return false;
    PreparedOnsetDetectionSequence next;
    if(!next.configure(frames,c,energy_frames?energy_frames:frames))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedOnsetDetectionSequence::configure(std::size_t frames,OnsetDetectionSequenceConfig c,std::size_t energy_frames) {
    if(!std::isfinite(c.sample_rate) || c.sample_rate<=0 || !c.hop_samples ||
        (c.sparse && unsigned(c.units)>unsigned(EventCoordinateUnit::Seconds)))return false;
    OnsetPeakConfig peaks;
    if(c.peaks)peaks=*c.peaks;
    else if(!onset_detection_parameters(c.sample_rate/float(c.hop_samples),peaks))return false;
    // The composition owns the global any/finite guard; raw peak_pick must
    // also process zero channels when another channel makes the batch active.
    peaks.suppress_empty=false;normalize_=peaks.normalize;
    if(!picker_.prepare(frames,peaks) || !backtracker_.prepare(energy_frames))return false;
    config_=c;maximum_frames_=frames;mask_.resize(frames);events_.resize(frames);backtracked_.resize(frames);
    samples_.resize(frames);seconds_.resize(frames);return true;
}
bool PreparedOnsetDetectionSequence::select(std::span<const float> envelope,std::optional<std::span<const float>> energy,std::size_t& count,bool batch_active) noexcept {
    count=0;
    if(!finite_normalized_envelope(envelope,normalize_))return true;
    if(!batch_active && !envelope_activity(envelope,normalize_))return true;
    auto mask=std::span(mask_).first(envelope.size());
    if(!picker_.process(envelope,mask))return false;
    for(std::size_t i=0;i<mask.size();++i)if(mask[i])events_[count++]=i;
    if(config_.backtrack && !config_.sparse &&
        std::any_of(picker_.processed_envelope().begin(),picker_.processed_envelope().end(),[](float x){return x!=0;}))return false;
    if(!config_.backtrack || !count)return true;
    const auto values=energy.value_or(picker_.processed_envelope());
    if(!backtracker_.process(std::span(events_).first(count),values,std::span(backtracked_).first(count)))return false;
    std::copy_n(backtracked_.begin(),count,events_.begin());return true;
}
bool PreparedOnsetDetectionSequence::result(std::size_t frames,std::size_t count,OnsetDetectionResult& output) noexcept {
    OnsetDetectionResult value;
    if(!config_.sparse)value.dense=std::span(mask_).first(frames);
    else if(config_.units==EventCoordinateUnit::Frames)value.frames=std::span(events_).first(count);
    else if(config_.units==EventCoordinateUnit::Samples){
        for(std::size_t i=0;i<count;++i)if(!event_sample_position(events_[i],config_.hop_samples,samples_[i]))return false;
        value.samples=std::span(samples_).first(count);
    } else {
        for(std::size_t i=0;i<count;++i)if(!event_time_position(events_[i],config_.hop_samples,config_.sample_rate,seconds_[i]))return false;
        value.seconds=std::span(seconds_).first(count);
    }
    output=value;return true;
}
bool PreparedOnsetDetectionSequence::process(std::span<const float> envelope,OnsetDetectionResult& output,std::optional<std::span<const float>> energy) noexcept {
    return process_active(envelope,output,energy,false);
}
bool PreparedOnsetDetectionSequence::process_active(std::span<const float> envelope,OnsetDetectionResult& output,
    std::optional<std::span<const float>> energy,bool batch_active) noexcept {
    if(!maximum_frames_ || envelope.size()>maximum_frames_ || (envelope.empty() && normalize_))return false;
    std::fill_n(mask_.begin(),envelope.size(),std::uint8_t{0});
    std::size_t count=0;
    return select(envelope,energy,count,batch_active) && result(envelope.size(),count,output);
}
bool PreparedOnsetDetectionSequence::process_channels(std::span<const std::span<const float>> channels,OnsetDetectionDestination output) noexcept {
    if(!output.write || (config_.sparse && channels.size()>1))return false;
    bool suppressed=false;
    if(!batch_suppressed(channels,suppressed))return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        OnsetDetectionResult result;
        const bool ok=suppressed?empty_result(channels[channel].size(),result):process_active(channels[channel],result,std::nullopt,true);
        if(!ok || !output.write(output.context,channel,result))return false;
    }
    return true;
}
bool PreparedOnsetDetectionSequence::empty_result(std::size_t frames,OnsetDetectionResult& output) noexcept {
    std::fill_n(mask_.begin(),frames,std::uint8_t{0});
    return result(frames,0,output);
}
bool PreparedOnsetDetectionSequence::batch_suppressed(std::span<const std::span<const float>> channels,bool& suppressed) const noexcept {
    if(!maximum_frames_)return false;
    suppressed=false;bool active=false;
    for(const auto values:channels){
        if(values.size()>maximum_frames_ || (values.empty() && normalize_))return false;
        suppressed=suppressed || !finite_normalized_envelope(values,normalize_);
        active=active || envelope_activity(values,normalize_);
    }
    suppressed=suppressed || !active;
    return suppressed || !config_.backtrack || config_.sparse;
}
}


namespace dsp_kernels {
bool PreparedOnsetDetectionSequence::storage_bytes(std::size_t n,OnsetDetectionSequenceConfig c,
    std::size_t energy,std::size_t& out) noexcept {
    if(!std::isfinite(c.sample_rate) || c.sample_rate<=0 || !c.hop_samples ||
       (c.sparse && unsigned(c.units)>unsigned(EventCoordinateUnit::Seconds)))return false;
    OnsetPeakConfig peaks;
    if(c.peaks)peaks=*c.peaks;
    else if(!onset_detection_parameters(c.sample_rate/float(c.hop_samples),peaks))return false;
    std::size_t bytes=0,nested=0;
    if(!PreparedOnsetPeakPicker::storage_bytes(n,peaks,bytes) ||
       !PreparedOnsetBacktracker::storage_bytes(energy?energy:n,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,n,sizeof(std::uint8_t)+2*sizeof(std::size_t)+sizeof(std::uint64_t)+sizeof(double)))return false;
    out=bytes;return true;
}
std::size_t PreparedOnsetDetectionSequence::workspace_bytes() const noexcept {
    return picker_.workspace_bytes()+backtracker_.workspace_bytes()+mask_.capacity()+
        (events_.capacity()+backtracked_.capacity())*sizeof(std::size_t)+samples_.capacity()*sizeof(std::uint64_t)+seconds_.capacity()*sizeof(double);
}
}
