// SPDX-License-Identifier: ISC
// librosa beat.py plp, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/envelope_local_pulse.h>
#include <utility>
namespace dsp_kernels {
bool PreparedEnvelopeLocalPulse::prepare(std::size_t maximum,EnvelopeLocalPulseConfig c,std::span<const float> coefficients) {
    std::size_t required=0;if(!storage_bytes(maximum,c,coefficients,required))return false;
    if(!maximum || !c.hop_samples)return false;
    PreparedEnvelopeLocalPulse next;
    TempogramSequenceConfig tg;tg.method=TempogramMethod::Fourier;
    tg.frame_rate=c.sample_rate/float(c.hop_samples);tg.autocorrelation.window=c.pulse.window;
    if(!next.fourier_.prepare(tg,coefficients))return false;
    c.pulse.frame_rate=tg.frame_rate;c.pulse.window=next.fourier_.config().autocorrelation.window;
    if(!next.pulse_.prepare(maximum,c.pulse,coefficients))return false;
    next.maximum_frames_=maximum;next.batch_pulse_.resize(maximum);if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedEnvelopeLocalPulse::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& rows=*static_cast<const Rows*>(context);
    return rows.processor->process_frame(rows.envelope,frame,output);
}
bool PreparedEnvelopeLocalPulse::process(std::span<const float> envelope,std::span<float> output,std::span<const float> prior,FloatMatrixSource prior_rows) noexcept {
    if(!maximum_frames_ || envelope.size()>maximum_frames_ || output.size()!=envelope.size())return false;
    Rows rows{&fourier_,envelope};
    return pulse_.process({&rows,read},fourier_.frames(envelope.size()),output,prior,prior_rows);
}
bool PreparedEnvelopeLocalPulse::process_channels(std::span<const std::span<const float>> channels,
    FloatMatrixBatchDestination output,std::span<const float> prior) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel)
        if(!process_channel(channels[channel],channel,output,prior,{}))return false;
    return true;
}
bool PreparedEnvelopeLocalPulse::process_channel(std::span<const float> envelope,std::size_t channel,
    FloatMatrixBatchDestination output,std::span<const float> prior,FloatMatrixSource prior_rows) noexcept {
    const auto size=envelope.size();
    if(size>batch_pulse_.size() || !process(envelope,std::span(batch_pulse_).first(size),prior,prior_rows))return false;
    for(std::size_t frame=0;frame<size;++frame)
        if(!output.write(output.context,channel,frame,std::span(batch_pulse_).subspan(frame,1)))return false;
    return true;
}
bool PreparedEnvelopeLocalPulse::process_channels(std::span<const LocalPulseChannelInput> channels,FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];
        if(!process_channel(input.envelope,channel,output,input.log_prior,input.log_prior_rows))return false;
    }
    return true;
}
}


namespace dsp_kernels {
bool PreparedEnvelopeLocalPulse::storage_bytes(std::size_t maximum,EnvelopeLocalPulseConfig c,std::span<const float> coeff,std::size_t& out) noexcept {
    if(!maximum || !c.hop_samples)return false;
    TempogramSequenceConfig tg;tg.method=TempogramMethod::Fourier;tg.frame_rate=c.sample_rate/float(c.hop_samples);
    tg.autocorrelation.window=c.pulse.window;tg.autocorrelation.window.empty_centered_frame=true;
    c.pulse.frame_rate=tg.frame_rate;c.pulse.window=tg.autocorrelation.window;
    std::size_t bytes=0,nested=0;
    if(!PreparedTempogramSequence::storage_bytes(tg,coeff,bytes) || !PreparedLocalPulse::storage_bytes(maximum,c.pulse,coeff,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,maximum,sizeof(float)))return false;
    out=bytes;return true;
}
}
