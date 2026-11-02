// SPDX-License-Identifier: ISC
// librosa feature/rhythm.py PCM paths, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/pcm_tempo.h>
#include <utility>
namespace dsp_kernels {
bool PreparedPcmTempogram::prepare(std::size_t maximum,PcmTempogramConfig c,
    std::span<const float> spectral_window,std::span<const float> temporal_window) {
    std::size_t required=0;if(!storage_bytes(maximum,c,spectral_window,temporal_window,required))return false;
    if(!c.onset.onset.strength.hop_length || c.tempogram.frame_rate!=c.onset.spectrum.sample_rate/float(c.onset.onset.strength.hop_length))return false;
    PreparedPcmTempogram next;
    if(!next.onset_.prepare(maximum,c.onset,spectral_window) || !next.tempogram_.prepare(c.tempogram,temporal_window))return false;
    next.row_.resize(next.tempogram_.columns());if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmTempogram::process(std::span<const float> pcm,FloatMatrixDestination output) noexcept {
    return onset_.process(pcm) && tempogram_.process(onset_.values(),output);
}
bool PreparedPcmTempogram::process_channel(std::span<const float> pcm,std::size_t channel,FloatMatrixBatchDestination output) noexcept {
    if(!onset_.process(pcm))return false;
    const auto count=tempogram_.frames(onset_.values().size());
    if(!count)return false;
    for(std::size_t frame=0;frame<count;++frame){
        if(!tempogram_.process_frame(onset_.values(),frame,row_) ||
            !output.write(output.context,channel,frame,row_))return false;
    }
    return true;
}
bool PreparedPcmTempogram::process_channels(std::span<const std::span<const float>> channels,FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel)
        if(!process_channel(channels[channel],channel,output))return false;
    return true;
}
bool PreparedPcmTempo::prepare(std::size_t maximum,PcmTempoConfig c,std::span<const float> window) {
    std::size_t required=0;if(!storage_bytes(maximum,c,window,required))return false;
    if(c.tempo.sample_rate!=c.onset.spectrum.sample_rate || c.tempo.hop_samples!=c.onset.onset.strength.hop_length)return false;
    PreparedPcmTempo next;
    if(!next.onset_.prepare(maximum,c.onset,window) || !next.tempo_.prepare(next.onset_.frames(maximum),c.tempo))return false;
    next.batch_bpm_.resize(next.onset_.frames(maximum));if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmTempo::process_channels(std::span<const std::span<const float>> channels,
    FloatMatrixBatchDestination output,std::span<const float> prior,FloatMatrixReductionPort reduction) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto count=output_frames(channels[channel].size(),reduction);
        if(!count || !process(channels[channel],std::span(batch_bpm_).first(count),prior,reduction))return false;
        for(std::size_t frame=0;frame<count;++frame)
            if(!output.write(output.context,channel,frame,std::span(batch_bpm_).subspan(frame,1)))return false;
    }
    return true;
}
bool PreparedPcmTempo::process(std::span<const float> pcm,std::span<float> output,
    std::span<const float> prior,FloatMatrixReductionPort reduction) noexcept {
    return onset_.process(pcm) && tempo_.process(onset_.values(),output,prior,reduction);
}
}


namespace dsp_kernels {
bool PreparedPcmTempogram::storage_bytes(std::size_t maximum,PcmTempogramConfig c,std::span<const float> sw,std::span<const float> tw,std::size_t& out) noexcept {
    const auto hop=c.onset.onset.strength.hop_length;
    if(!hop || c.tempogram.frame_rate!=c.onset.spectrum.sample_rate/float(hop))return false;
    std::size_t bytes=0,nested=0,frames=0;
    const auto bins=c.tempogram.autocorrelation.window.length/2+1;
    const auto cols=c.tempogram.method==TempogramMethod::Autocorrelation?c.tempogram.autocorrelation.window.length:
        c.tempogram.method==TempogramMethod::Fourier?2*bins:bins;
    if(!PreparedPcmOnsetEnvelope::geometry(maximum,c.onset,{},frames) || !PreparedPcmOnsetEnvelope::storage_bytes(maximum,c.onset,sw,{},bytes) ||
       !PreparedTempogramSequence::storage_bytes(c.tempogram,tw,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,cols,sizeof(float)))return false;
    out=bytes;return true;
}
bool PreparedPcmTempo::storage_bytes(std::size_t maximum,PcmTempoConfig c,std::span<const float> window,std::size_t& out) noexcept {
    if(c.tempo.sample_rate!=c.onset.spectrum.sample_rate || c.tempo.hop_samples!=c.onset.onset.strength.hop_length)return false;
    std::size_t bytes=0,nested=0,frames=0;
    if(!PreparedPcmOnsetEnvelope::geometry(maximum,c.onset,{},frames) || !PreparedPcmOnsetEnvelope::storage_bytes(maximum,c.onset,window,{},bytes) ||
       !PreparedEnvelopeTempo::storage_bytes(frames,c.tempo,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,frames,sizeof(float)))return false;
    out=bytes;return true;
}
}
