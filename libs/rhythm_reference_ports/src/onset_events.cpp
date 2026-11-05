// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia Onsets::compute composition at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/onset_events.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
bool PreparedOnsetEvents::prepare(std::size_t features,std::size_t frames,AdaptiveOnsetSelectionConfig config) {
    PreparedOnsetEvents next;
    std::size_t bytes=0;if(!storage_bytes(features,frames,config,bytes))return false;
    if(!next.fusion_.prepare(features,frames) || !next.average_.prepare(config.delay) || !next.selection_.prepare(frames,config))return false;
    next.maximum_frames_=frames;next.fused_.resize(frames);next.smoothed_.resize(frames);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedOnsetEvents::storage_bytes(std::size_t features,std::size_t frames,AdaptiveOnsetSelectionConfig config,std::size_t& output) noexcept {
    std::size_t bytes=0,nested=0;using namespace dsp_kernels::prepared_storage;
    if(!PreparedNormalizedFeatureFusion::storage_bytes(features,frames,bytes) || !PreparedCausalAverage::storage_bytes(config.delay,nested) ||
       !add_bytes(bytes,nested) || !PreparedAdaptiveOnsetSelection::storage_bytes(frames,config,nested) || !add_bytes(bytes,nested) ||
       !add_array(bytes,frames,2*sizeof(float)))return false;
    output=bytes;return true;
}
void PreparedOnsetEvents::reset() noexcept {
    average_.reset();fusion_.reset();selection_.reset();result_frames_=0;
}
bool PreparedOnsetEvents::process(dsp_kernels::FloatMatrixSource source,std::size_t features,std::size_t frames,std::span<const float> weights,std::span<std::uint8_t> output,bool reset_filter) noexcept {
    result_frames_=0;selection_.reset();
    if(!maximum_frames_ || frames>maximum_frames_ || output.size()!=frames)return false;
    if(!std::all_of(weights.begin(),weights.end(),[](float x){return std::isfinite(x);}))return false;
    const float total=std::accumulate(weights.begin(),weights.end(),0.f);if(!std::isfinite(total))return false;
    if(!fusion_.process(source,features,frames,weights,std::span(fused_).first(frames)))return false;
    if(reset_filter)average_.reset();
    if(!average_.process(std::span(fused_).first(frames),std::span(smoothed_).first(frames)))return false;
    if(!selection_.process(std::span(smoothed_).first(frames),total,output))return false;
    result_frames_=frames;return true;
}
}
