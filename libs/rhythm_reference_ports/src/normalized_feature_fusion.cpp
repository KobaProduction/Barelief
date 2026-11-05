// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia Onsets::compute normalization/weighted fusion at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/normalized_feature_fusion.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
bool PreparedNormalizedFeatureFusion::prepare(std::size_t features,std::size_t frames) {
    std::size_t bytes=0;if(!storage_bytes(features,frames,bytes))return false;
    PreparedNormalizedFeatureFusion next;next.maximum_features_=features;next.maximum_frames_=frames;
    next.row_.resize(frames);next.result_.resize(frames);next.maxima_.resize(features);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedNormalizedFeatureFusion::storage_bytes(std::size_t features,std::size_t frames,std::size_t& output) noexcept {
    std::size_t bytes=0;
    if(!features || !frames || !dsp_kernels::prepared_storage::add_array(bytes,frames,2*sizeof(float)) ||
       !dsp_kernels::prepared_storage::add_array(bytes,features,sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedNormalizedFeatureFusion::accumulate(dsp_kernels::FloatMatrixSource source,std::size_t feature,std::size_t frames,float weight) noexcept {
    auto row=std::span(row_).first(frames);
    if(!source.read(source.context,feature,row) || !std::all_of(row.begin(),row.end(),[](float x){return std::isfinite(x);}))return false;
    const float maximum=row.empty()?0:*std::max_element(row.begin(),row.end());maxima_[feature]=maximum;
    for(std::size_t i=0;i<frames;++i){
        const float normalized=maximum!=0?row[i]/maximum:row[i];
        result_[i]+=weight*normalized;
        if(!std::isfinite(result_[i]))return false;
    }
    return true;
}
bool PreparedNormalizedFeatureFusion::process(dsp_kernels::FloatMatrixSource source,std::size_t features,std::size_t frames,std::span<const float> weights,std::span<float> output) noexcept {
    result_features_=0;
    if(!maximum_features_ || !source.read || !features || features>maximum_features_ || weights.size()!=features || frames>maximum_frames_ || output.size()!=frames)return false;
    if(!std::all_of(weights.begin(),weights.end(),[](float x){return std::isfinite(x);}))return false;
    std::fill_n(result_.begin(),frames,0.f);
    for(std::size_t feature=0;feature<features;++feature)if(!accumulate(source,feature,frames,weights[feature]))return false;
    std::copy_n(result_.begin(),frames,output.begin());result_features_=features;return true;
}
}
