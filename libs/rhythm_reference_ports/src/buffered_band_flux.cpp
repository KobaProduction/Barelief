// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia SuperFluxNovelty at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/buffered_band_flux.h>
#include <cmath>
#include <utility>
#include <dsp_kernels/prepared_storage.h>

namespace rhythm_reference_ports {
bool PreparedBufferedBandFlux::prepare(std::size_t bands,std::size_t frames,BufferedBandFluxConfig config) {
    std::size_t bytes=0;if(!storage_bytes(bands,frames,config,bytes))return false;
    PreparedBufferedBandFlux next;
    dsp_kernels::OnsetStrengthConfig difference;difference.center=false;difference.aggregate=dsp_kernels::OnsetAggregate::Sum;
    if(!next.maximum_.prepare(config.bin_width,false) || !next.difference_.prepare(bands,1,difference))return false;
    next.config_=config;next.maximum_frames_=frames;
    next.current_.resize(bands);next.delayed_.resize(bands);next.reference_.resize(bands);next.differences_.resize(bands);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedBufferedBandFlux::storage_bytes(std::size_t bands,std::size_t frames,BufferedBandFluxConfig config,std::size_t& output) noexcept {
    if(!bands || frames<=config.frame_width || !config.frame_width || config.bin_width<3)return false;
    dsp_kernels::OnsetStrengthConfig difference;difference.center=false;difference.aggregate=dsp_kernels::OnsetAggregate::Sum;
    std::size_t bytes=0,maximum=0;
    if(!dsp_kernels::PreparedOnsetStrength::storage_bytes(bands,1,difference,bytes) ||
       !PreparedBufferedMaximum::storage_bytes(config.bin_width,maximum) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,maximum) ||
       !dsp_kernels::prepared_storage::add_array(bytes,bands,4*sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedBufferedBandFlux::workspace_bytes() const noexcept {
    return maximum_.workspace_bytes()+difference_.workspace_bytes()+
        (current_.capacity()+delayed_.capacity()+reference_.capacity()+differences_.capacity())*sizeof(float);
}
void PreparedBufferedBandFlux::reset(bool clear,BufferedMaximumReset mode) noexcept {
    if(clear)maximum_.reset(mode);
}
bool PreparedBufferedBandFlux::accumulate(std::span<const float> current,std::span<const float> delayed,float& result) noexcept {
    if(current.size()!=current_.size() || delayed.size()!=delayed_.size() || !maximum_frames_)return false;
    if(!maximum_.process(delayed,reference_) ||
       !difference_.positive_differences(current,reference_,differences_,false))return false;
    for(const float value:differences_)result+=value;
    return std::isfinite(result);
}
bool PreparedBufferedBandFlux::process_pair(std::span<const float> current,std::span<const float> delayed,float& novelty) noexcept {
    float result=0;
    if(!accumulate(current,delayed,result))return false;
    novelty=result;return true;
}
bool PreparedBufferedBandFlux::process(dsp_kernels::FloatMatrixSource source,std::size_t frames,float& novelty) noexcept {
    if(!source.read || frames<=config_.frame_width || frames>maximum_frames_)return false;
    float result=0;
    for(std::size_t i=config_.frame_width;i<frames;++i)
        if(!source.read(source.context,i,current_) || !source.read(source.context,i-config_.frame_width,delayed_) ||
           !accumulate(current_,delayed_,result))return false;
    novelty=result;return true;
}
}
