// SPDX-License-Identifier: ISC
// librosa onset.py onset_strength(_multi) PCM composition,
// librosa/librosa@3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full license: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <dsp_kernels/pcm_onset_strength.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
SpectralFrameRecipeConfig mel_power_spectral_parameters(float rate,std::size_t fft,std::size_t hop) noexcept {
    SpectralFrameRecipeConfig c;c.sample_rate=rate;
    c.framing.length=fft;c.framing.hop_samples=double(hop);c.framing.tail=FrameSamplingTail::Complete;
    c.periodic=true;c.include_nyquist=true;c.power=2;c.physical_filter_axis=true;
    c.filtering=SpectralFilterMode::Mel;
    c.mel_filters.bands=128;c.mel_filters.fmin_hz=0;c.mel_filters.fmax_hz=0;
    c.mel_filters.scale=MelScale::Slaney;c.mel_filters.normalization=MelNormalization::Slaney;
    c.mel_filters.construction=MelConstruction::Continuous;return c;
}
PcmOnsetStrengthConfig median_onset_parameters() noexcept {
    PcmOnsetStrengthConfig c;c.onset.strength.aggregate=OnsetAggregate::Median;return c;
}
bool PreparedPcmOnsetStrength::prepare(std::size_t maximum,PcmOnsetStrengthConfig c,
    std::span<const float> window,ExplicitSpectralFilters filters,
    std::span<const OnsetFeatureGroup> groups,OnsetAggregationPort reducer) {
    std::size_t required=0;if(!storage_bytes(maximum,c,window,filters,groups,required))return false;
    PreparedPcmOnsetStrength next;
    if(c.spectrum.framing.length!=c.onset.strength.n_fft ||
        (c.spectrum.fft_size && c.spectrum.fft_size!=c.onset.strength.n_fft) ||
        c.spectrum.framing.hop_samples!=double(c.onset.strength.hop_length) ||
        c.spectrum.frame_rate!=0 || c.spectrum.logarithm)return false;
    if(!next.spectrum_.prepare(maximum,c.spectrum,window,filters))return false;
    const auto frames=next.spectrum_.frames(maximum);next.features_=next.spectrum_.features();
    if(!next.decibels_.prepare(frames,next.features_,c.decibels) ||
        !next.onset_.prepare(next.features_,frames,c.onset,groups,reducer))return false;
    next.maximum_samples_=maximum;if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmOnsetStrength::read_power(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& rows=*static_cast<const Rows*>(context);
    if(!rows.owner->spectrum_.process_frame(rows.pcm,frame,output))return false;
    for(auto& value:output)value=std::abs(value);
    return true;
}
bool PreparedPcmOnsetStrength::read_db(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& rows=*static_cast<const Rows*>(context);
    return read_power(context,frame,output) && rows.owner->decibels_.process_row(frame,output,output);
}
bool PreparedPcmOnsetStrength::process(std::span<const float> pcm,FloatMatrixDestination output,FloatMatrixSource reference) noexcept {
    if(!maximum_samples_ || pcm.size()>maximum_samples_ || !output.write ||
        !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    const auto count=spectrum_.frames(pcm.size());Rows rows{this,pcm};
    if(!count)return false;
    if(!decibels_.measure_reference({&rows,read_power},count,features_))return false;
    return onset_.process({&rows,read_db},count,output,reference);
}
bool PreparedPcmOnsetStrength::process_channels(std::span<const std::span<const float>> channels,
    std::span<const FloatMatrixDestination> outputs) noexcept {
    if(channels.size()!=outputs.size())return false;
    for(std::size_t i=0;i<channels.size();++i)if(!process(channels[i],outputs[i]))return false;
    return true;
}
}


namespace dsp_kernels {
bool PreparedPcmOnsetStrength::storage_bytes(std::size_t maximum,PcmOnsetStrengthConfig c,std::span<const float> window,
    ExplicitSpectralFilters filters,std::span<const OnsetFeatureGroup> groups,std::size_t& out) noexcept {
    if(c.spectrum.framing.length!=c.onset.strength.n_fft || (c.spectrum.fft_size && c.spectrum.fft_size!=c.onset.strength.n_fft) ||
       c.spectrum.framing.hop_samples!=double(c.onset.strength.hop_length) || c.spectrum.frame_rate!=0 || c.spectrum.logarithm)return false;
    std::size_t frames=0,features=0,bytes=0,nested=0;
    if(!PreparedSpectralFrameRecipe::geometry(maximum,c.spectrum,filters,frames,features) ||
       !PreparedSpectralFrameRecipe::storage_bytes(maximum,c.spectrum,window,filters,bytes) ||
       !PreparedPowerToDb::storage_bytes(frames,features,c.decibels,nested))return false;
    nested-=sizeof(PreparedPowerToDb);
    if(!prepared_storage::add_bytes(bytes,nested) || !PreparedOnsetStrengthSequence::storage_bytes(features,frames,c.onset,groups,nested) ||
       !prepared_storage::add_bytes(bytes,nested))return false;
    out=bytes;return true;
}
std::size_t PreparedPcmOnsetStrength::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+decibels_.workspace_bytes()+onset_.workspace_bytes();
}
}
