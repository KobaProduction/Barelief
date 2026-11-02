// SPDX-License-Identifier: ISC
// librosa onset_strength PCM composition, pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <dsp_kernels/pcm_onset_envelope.h>
#include <utility>
namespace dsp_kernels {
bool PreparedPcmOnsetEnvelope::prepare(std::size_t maximum,PcmOnsetStrengthConfig c,
    std::span<const float> window,ExplicitSpectralFilters filters,OnsetAggregationPort reducer) {
    std::size_t required=0;if(!storage_bytes(maximum,c,window,filters,required))return false;
    PreparedPcmOnsetEnvelope next;
    if(!next.source_.prepare(maximum,c,window,filters,{},reducer) || next.source_.output_features()!=1)return false;
    next.values_.resize(next.source_.frames(maximum));if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmOnsetEnvelope::write(void* context,std::size_t time,std::span<const float> values) noexcept {
    auto& owner=*static_cast<PreparedPcmOnsetEnvelope*>(context);
    if(values.size()!=1 || time>=owner.values_.size())return false;
    owner.values_[time]=values[0];return true;
}
bool PreparedPcmOnsetEnvelope::write_into(void* context,std::size_t time,std::span<const float> values) noexcept {
    auto& output=*static_cast<std::span<float>*>(context);
    if(values.size()!=1 || time>=output.size())return false;
    output[time]=values[0];return true;
}
bool PreparedPcmOnsetEnvelope::process_into(std::span<const float> pcm,std::span<float> output,FloatMatrixSource reference) noexcept {
    count_=0;
    if(output.size()!=source_.frames(pcm.size()))return false;
    return source_.process(pcm,{&output,write_into},reference);
}
bool PreparedPcmOnsetEnvelope::process(std::span<const float> pcm,FloatMatrixSource reference) noexcept {
    count_=0;
    if(!source_.process(pcm,{this,write},reference))return false;
    count_=source_.frames(pcm.size());return true;
}
}


namespace dsp_kernels {
bool PreparedPcmOnsetEnvelope::geometry(std::size_t maximum,PcmOnsetStrengthConfig c,ExplicitSpectralFilters filters,std::size_t& out) noexcept {
    std::size_t frames=0,features=0;
    if(!PreparedSpectralFrameRecipe::geometry(maximum,c.spectrum,filters,frames,features) ||
       (!c.onset.aggregate_features && features!=1))return false;
    out=c.onset.strength.center?frames:std::max(frames,c.onset.strength.lag);return true;
}
bool PreparedPcmOnsetEnvelope::storage_bytes(std::size_t maximum,PcmOnsetStrengthConfig c,std::span<const float> window,
    ExplicitSpectralFilters filters,std::size_t& out) noexcept {
    std::size_t bytes=0,count=0;
    if(!geometry(maximum,c,filters,count) || !PreparedPcmOnsetStrength::storage_bytes(maximum,c,window,filters,{},bytes) ||
       !prepared_storage::add_array(bytes,count,sizeof(float)))return false;
    out=bytes;return true;
}
}
