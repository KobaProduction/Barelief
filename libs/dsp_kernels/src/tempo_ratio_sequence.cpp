// SPDX-License-Identifier: ISC
// librosa feature/rhythm.py tempogram_ratio / core/harmonic.py f0_harmonics,
// pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/tempo_ratio_sequence.h>
#include <dsp_kernels/tempogram.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
bool PreparedTempoRatioSequence::prepare(std::size_t frames,std::size_t bins,TempoRatioSequenceConfig c,
    std::span<const float> coordinates,std::optional<std::span<const float>> factors) {
    std::size_t required=0;if(!storage_bytes(frames,bins,c,factors?factors->size():13,required))return false;
    PreparedTempoRatioSequence next;
    if(!next.configure(frames,bins,c,coordinates,factors))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedTempoRatioSequence::prepare_axes(std::size_t bins,TempoRatioSequenceConfig c,
    std::span<const float> supplied,std::optional<std::span<const float>> factors) {
    constexpr std::array defaults{4.f,8.f/3.f,3.f,2.f,4.f/3.f,1.5f,1.f,2.f/3.f,.75f,.5f,1.f/3.f,3.f/8.f,.25f};
    const auto ratios=factors.value_or(std::span<const float>(defaults));
    factors_.assign(ratios.begin(),ratios.end());coordinates_.resize(bins);
    if(supplied.empty())return autocorrelation_tempo_frequencies(c.tempo.frame_rate,coordinates_);
    std::copy(supplied.begin(),supplied.end(),coordinates_.begin());return true;
}
bool PreparedTempoRatioSequence::configure(std::size_t frames,std::size_t bins,TempoRatioSequenceConfig c,
    std::span<const float> supplied,std::optional<std::span<const float>> factors) {
    if(!frames || !bins || !std::isfinite(c.fill) || (!supplied.empty() && supplied.size()!=bins))return false;
    if(!prepare_axes(bins,c,supplied,factors))return false;
    c.tempo.aggregate=TempoAggregation::None;c.interpolation.keep_infinite_coordinates=false;c.interpolation.source_linear=true;
    c.interpolation.fill_below=c.fill;c.interpolation.fill_above=c.fill;
    config_=c;maximum_frames_=frames;bins_=bins;
    const bool curve=c.dynamic_frequencies?interpolation_.prepare_dynamic(bins,c.interpolation):interpolation_.prepare(coordinates_,c.interpolation);
    if(!curve || !tempo_.prepare(frames,bins,c.tempo) || !reduction_.prepare(frames,factors_.size(),c.time_reduction))return false;
    input_.resize(bins);bpm_.resize(frames);return true;
}
bool PreparedTempoRatioSequence::read(const void* context,std::size_t time,std::span<float> output) noexcept {
    const auto& rows=*static_cast<const Rows*>(context);auto& owner=*rows.owner;
    if(!rows.values.read(rows.values.context,time,owner.input_))return false;
    if(owner.config_.dynamic_frequencies){
        if(!rows.frequencies.read(rows.frequencies.context,time,owner.coordinates_) ||
            !owner.interpolation_.set_coordinates(owner.coordinates_))return false;
    }
    return owner.interpolation_.set_values(owner.input_) &&
        tempogram_ratios(owner.interpolation_,rows.bpm[rows.bpm.size()==1?0:time],owner.factors_,output,owner.config_.fill);
}
bool PreparedTempoRatioSequence::process_channels(std::span<const TempoRatioChannelInput> channels,FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];FloatMatrixBatchChannel sink{output,channel};
        if(!process(input.values,input.frames,sink.destination(),input.bpm,input.log_prior,input.frequencies,input.reduction))return false;
    }
    return true;
}
bool PreparedTempoRatioSequence::process(FloatMatrixSource source,std::size_t frames,FloatMatrixDestination output,
    std::optional<std::span<const float>> supplied,std::span<const float> prior,
    FloatMatrixSource frequencies,FloatMatrixReductionPort reduction) noexcept {
    if(!maximum_frames_ || !frames || frames>maximum_frames_ || !source.read || !output.write ||
        (config_.dynamic_frequencies && !frequencies.read))return false;
    std::span<const float> bpm;
    if(supplied){bpm=*supplied;if(bpm.size()!=1 && bpm.size()!=frames)return false;}
    else {
        if(!tempo_.process(source,frames,std::span(bpm_).first(frames),prior))return false;
        bpm=std::span(bpm_).first(frames);
    }
    Rows rows{this,source,frequencies,bpm};
    return reduction_.process({&rows,read},frames,output,reduction);
}
}


namespace dsp_kernels {
bool PreparedTempoRatioSequence::storage_bytes(std::size_t frames,std::size_t bins,TempoRatioSequenceConfig c,std::size_t factors,std::size_t& out) noexcept {
    if(!frames || !bins || !std::isfinite(c.fill))return false;
    c.tempo.aggregate=TempoAggregation::None;c.interpolation.keep_infinite_coordinates=false;c.interpolation.source_linear=true;
    c.interpolation.fill_below=c.fill;c.interpolation.fill_above=c.fill;
    std::size_t bytes=0,nested=0;
    if(!PreparedTempoEstimator::storage_bytes(frames,bins,c.tempo,bytes) ||
       !PreparedCurveInterpolation::storage_bytes(bins,c.interpolation,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedFeatureReduction::storage_bytes(frames,factors,c.time_reduction,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,bins,2*sizeof(float)) || !prepared_storage::add_array(bytes,factors,sizeof(float)) ||
       !prepared_storage::add_array(bytes,frames,sizeof(float)))return false;
    if(c.dynamic_frequencies && !prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedTempoRatioSequence::workspace_bytes() const noexcept {
    return tempo_.workspace_bytes()+interpolation_.workspace_bytes()+reduction_.workspace_bytes()+
        (coordinates_.capacity()+factors_.capacity()+input_.capacity()+bpm_.capacity())*sizeof(float);
}
}
