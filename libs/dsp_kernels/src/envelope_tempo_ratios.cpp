// SPDX-License-Identifier: ISC
// librosa feature/rhythm.py tempogram_ratio input compositions,
// pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/envelope_tempo_ratios.h>
#include <utility>
namespace dsp_kernels {
bool PreparedEnvelopeTempoRatios::prepare(std::size_t maximum,EnvelopeTempoRatiosConfig c,
    std::span<const float> coefficients,std::span<const float> coordinates,
    std::optional<std::span<const float>> factors) {
    std::size_t required=0;if(!storage_bytes(maximum,c,coefficients,coordinates,factors,required))return false;
    if(!maximum || c.tempogram.method!=TempogramMethod::Autocorrelation ||
        c.tempogram.frame_rate!=c.ratios.tempo.frame_rate)return false;
    PreparedEnvelopeTempoRatios next;
    if(!next.tempogram_.prepare(c.tempogram,coefficients))return false;
    if(!next.ratios_.prepare(next.tempogram_.frames(maximum),next.tempogram_.columns(),c.ratios,coordinates,factors))return false;
    next.maximum_frames_=maximum;if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedEnvelopeTempoRatios::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& rows=*static_cast<const Rows*>(context);
    return rows.processor->process_frame(rows.envelope,frame,output);
}
bool PreparedEnvelopeTempoRatios::process(std::span<const float> envelope,FloatMatrixDestination output,
    std::optional<std::span<const float>> bpm,std::span<const float> prior,
    FloatMatrixSource frequencies,FloatMatrixReductionPort reduction) noexcept {
    if(!maximum_frames_ || envelope.size()>maximum_frames_)return false;
    Rows rows{&tempogram_,envelope};
    return ratios_.process({&rows,read},tempogram_.frames(envelope.size()),output,bpm,prior,frequencies,reduction);
}
bool PreparedEnvelopeTempoRatios::process_channels(std::span<const EnvelopeTempoRatioChannelInput> channels,
    FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];FloatMatrixBatchChannel sink{output,channel};
        if(!process(input.envelope,sink.destination(),input.bpm,input.log_prior,input.frequencies,input.reduction))return false;
    }
    return true;
}
bool PreparedPcmTempoRatios::prepare(std::size_t maximum,PcmTempoRatiosConfig c,
    std::span<const float> spectral_window,std::span<const float> temporal_window,
    std::span<const float> coordinates,std::optional<std::span<const float>> factors) {
    std::size_t required=0;if(!storage_bytes(maximum,c,spectral_window,temporal_window,coordinates,factors,required))return false;
    const auto hop=c.onset.onset.strength.hop_length;
    if(!hop || c.ratios.tempogram.frame_rate!=c.onset.spectrum.sample_rate/float(hop))return false;
    PreparedPcmTempoRatios next;
    if(!next.onset_.prepare(maximum,c.onset,spectral_window) ||
        !next.ratios_.prepare(next.onset_.frames(maximum),c.ratios,temporal_window,coordinates,factors))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmTempoRatios::process(std::span<const float> pcm,FloatMatrixDestination output,
    std::optional<std::span<const float>> bpm,std::span<const float> prior,
    FloatMatrixSource frequencies,FloatMatrixReductionPort reduction) noexcept {
    return onset_.process(pcm) && ratios_.process(onset_.values(),output,bpm,prior,frequencies,reduction);
}
bool PreparedPcmTempoRatios::process_channels(std::span<const PcmTempoRatioChannelInput> channels,
    FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];FloatMatrixBatchChannel sink{output,channel};
        if(!process(input.pcm,sink.destination(),input.bpm,input.log_prior,input.frequencies,input.reduction))return false;
    }
    return true;
}
}


namespace dsp_kernels {
bool PreparedEnvelopeTempoRatios::storage_bytes(std::size_t maximum,EnvelopeTempoRatiosConfig c,std::span<const float> window,
    std::span<const float> coordinates,std::optional<std::span<const float>> factors,std::size_t& out) noexcept {
    if(!maximum || c.tempogram.method!=TempogramMethod::Autocorrelation || c.tempogram.frame_rate!=c.ratios.tempo.frame_rate ||
       (!coordinates.empty() && coordinates.size()!=c.tempogram.autocorrelation.window.length))return false;
    std::size_t frames=0,bytes=0,nested=0;
    if(!PreparedTempogramSequence::frame_count(c.tempogram,maximum,frames) || !PreparedTempogramSequence::storage_bytes(c.tempogram,window,bytes) ||
       !PreparedTempoRatioSequence::storage_bytes(frames,c.tempogram.autocorrelation.window.length,c.ratios,factors?factors->size():13,nested) ||
       !prepared_storage::add_bytes(bytes,nested))return false;
    out=bytes;return true;
}
bool PreparedPcmTempoRatios::storage_bytes(std::size_t maximum,PcmTempoRatiosConfig c,std::span<const float> sw,
    std::span<const float> tw,std::span<const float> coords,std::optional<std::span<const float>> factors,std::size_t& out) noexcept {
    const auto hop=c.onset.onset.strength.hop_length;
    if(!hop || c.ratios.tempogram.frame_rate!=c.onset.spectrum.sample_rate/float(hop))return false;
    std::size_t bytes=0,nested=0,frames=0;
    if(!PreparedPcmOnsetEnvelope::geometry(maximum,c.onset,{},frames) || !PreparedPcmOnsetEnvelope::storage_bytes(maximum,c.onset,sw,{},bytes) ||
       !PreparedEnvelopeTempoRatios::storage_bytes(frames,c.ratios,tw,coords,factors,nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    out=bytes;return true;
}
}
