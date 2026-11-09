// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia experimental Beatogram/Meter at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/beat_band_salience.h>
#include <dsp_kernels/order_statistics.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
void PreparedBeatBandSalience::allocate(std::size_t beats,std::size_t bands,std::size_t window) {
    maximum_beats_=beats;maximum_bands_=bands;window_size_=window;
    values_.resize(beats*bands);row_.resize(bands);scratch_.resize(std::min(beats,window));
    tick_thresholds_.resize(beats);band_thresholds_.resize(beats);
}
bool PreparedBeatBandSalience::prepare(std::size_t beats,std::size_t bands,std::size_t window) {
    if(!beats || !bands || !window || beats>SIZE_MAX/bands ||
       beats>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/2)return false;
    std::size_t bound=0;if(!storage_bytes(beats,bands,window,bound))return false;
    PreparedBeatBandSalience next;
    next.allocate(beats,bands,window);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
float PreparedBeatBandSalience::threshold(std::span<float> values) noexcept {
    const float mean=std::accumulate(values.begin(),values.end(),0.f)/float(values.size());
    return std::max(dsp_kernels::median_in_place(values),mean);
}
bool PreparedBeatBandSalience::read(dsp_kernels::FloatMatrixSource source,std::size_t beats,std::size_t bands) noexcept {
    auto row=std::span(row_).first(bands);
    for(std::size_t beat=0;beat<beats;++beat){
        if(!source.read(source.context,beat,row) ||
           !std::all_of(row.begin(),row.end(),[](float x){return std::isfinite(x);}))return false;
        for(std::size_t band=0;band<bands;++band)values_[band*maximum_beats_+beat]=row[band];
        tick_thresholds_[beat]=threshold(row);
        if(!std::isfinite(tick_thresholds_[beat]))return false;
    }
    return true;
}
bool PreparedBeatBandSalience::band_thresholds(std::span<const float> values) noexcept {
    const auto window=std::min(window_size_,values.size());
    for(std::size_t beat=0;beat<values.size();++beat){
        auto start=beat>window/2?beat-window/2:0;
        const auto end=std::min(values.size(),start+window);
        if(end==values.size())start=end-window;
        auto scratch=std::span(scratch_).first(end-start);
        std::copy(values.begin()+std::ptrdiff_t(start),values.begin()+std::ptrdiff_t(end),scratch.begin());
        band_thresholds_[beat]=threshold(scratch);
        if(!std::isfinite(band_thresholds_[beat]))return false;
    }
    return true;
}
bool PreparedBeatBandSalience::filter_bands(std::span<const float> energy,std::size_t bands) noexcept {
    for(std::size_t band=0;band<bands;++band)if(!filter_band(band,energy))return false;
    beats_=energy.size();bands_=bands;ready_=true;
    return true;
}
bool PreparedBeatBandSalience::filter_band(std::size_t band,std::span<const float> energy) noexcept {
    auto values=std::span(values_).subspan(band*maximum_beats_,energy.size());
    if(!band_thresholds(values))return false;
    for(std::size_t beat=0;beat<energy.size();++beat){
        if(values[beat]<band_thresholds_[beat] && values[beat]<=tick_thresholds_[beat])values[beat]=0;
        else values[beat]*=energy[beat];
        if(!std::isfinite(values[beat]))return false;
    }
    return true;
}
bool PreparedBeatBandSalience::process(dsp_kernels::FloatMatrixSource ratios,std::span<const float> energy,std::size_t bands) noexcept {
    ready_=false;beats_=0;bands_=0;
    if(!maximum_beats_ || energy.size()>maximum_beats_ || bands>maximum_bands_ ||
       !std::all_of(energy.begin(),energy.end(),[](float x){return std::isfinite(x);}))return false;
    if(energy.empty()){ready_=true;return true;}
    if(!bands || !ratios.read || !read(ratios,energy.size(),bands))return false;
    return filter_bands(energy,bands);
}
std::span<const float> PreparedBeatBandSalience::band_values(std::size_t band) const noexcept {
    if(!ready_ || band>=bands_)return {};
    return std::span(values_).subspan(band*maximum_beats_,beats_);
}
std::span<const float> PreparedBeatBandSalience::tick_thresholds() const noexcept {
    return ready_?std::span<const float>(tick_thresholds_).first(beats_):std::span<const float>{};
}
}
