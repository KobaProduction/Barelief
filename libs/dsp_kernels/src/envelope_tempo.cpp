// SPDX-License-Identifier: ISC
// librosa feature/rhythm.py tempo, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/envelope_tempo.h>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool tempo_window_frames(double seconds,float rate,std::size_t hop,std::size_t& output) noexcept {
    if(!std::isfinite(seconds) || seconds<0 || !std::isfinite(rate) || rate<=0 || !hop)return false;
    const double samples=seconds*double(rate);
    if(!std::isfinite(samples) || samples>=double(INT64_MAX))return false;
    const auto frames=std::uint64_t(samples)/hop;
    if(frames>SIZE_MAX)return false;
    output=std::size_t(frames);return output!=0;
}
bool PreparedEnvelopeTempo::prepare(std::size_t maximum,EnvelopeTempoConfig c) {
    std::size_t required=0;if(!storage_bytes(maximum,c,required))return false;
    std::size_t length=0;
    if(!maximum || !tempo_window_frames(c.autocorrelation_seconds,c.sample_rate,c.hop_samples,length))return false;
    PreparedEnvelopeTempo next;next.config_=c;
    TempogramSequenceConfig tg;tg.frame_rate=c.sample_rate/float(c.hop_samples);
    tg.autocorrelation.window.precision=c.precision;
    tg.autocorrelation.window.length=length;
    c.estimator.frame_rate=tg.frame_rate;next.config_.estimator=c.estimator;
    if(!next.tempogram_.prepare(tg) || !next.estimator_.prepare(maximum,length,c.estimator))return false;
    next.maximum_frames_=maximum;next.batch_bpm_.resize(maximum);if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedEnvelopeTempo::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& rows=*static_cast<const Rows*>(context);
    return rows.processor->process_frame(rows.envelope,frame,output);
}
std::size_t PreparedEnvelopeTempo::output_frames(std::size_t count,FloatMatrixReductionPort reduction) const noexcept {
    if(!maximum_frames_ || !count || count>maximum_frames_)return 0;
    return config_.estimator.aggregate==TempoAggregation::None && !reduction.reduce?count:1;
}
bool PreparedEnvelopeTempo::process_channels(std::span<const std::span<const float>> channels,
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
bool PreparedEnvelopeTempo::process(std::span<const float> envelope,std::span<float> output,
    std::span<const float> prior,FloatMatrixReductionPort reduction) noexcept {
    if(!maximum_frames_ || envelope.size()>maximum_frames_)return false;
    Rows rows{&tempogram_,envelope};
    return estimator_.process({&rows,read},tempogram_.frames(envelope.size()),output,prior,reduction);
}
}


namespace dsp_kernels {
bool PreparedEnvelopeTempo::storage_bytes(std::size_t maximum,EnvelopeTempoConfig c,std::size_t& out) noexcept {
    std::size_t length=0,bytes=0,nested=0;
    if(!maximum || !tempo_window_frames(c.autocorrelation_seconds,c.sample_rate,c.hop_samples,length))return false;
    TempogramSequenceConfig tg;tg.frame_rate=c.sample_rate/float(c.hop_samples);tg.autocorrelation.window.length=length;
    tg.autocorrelation.window.precision=c.precision;
    c.estimator.frame_rate=tg.frame_rate;
    if(!PreparedTempogramSequence::storage_bytes(tg,{},bytes) ||
       !PreparedTempoEstimator::storage_bytes(maximum,length,c.estimator,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,maximum,sizeof(float)))return false;
    out=bytes;return true;
}
}
