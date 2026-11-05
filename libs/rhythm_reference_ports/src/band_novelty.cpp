// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia NoveltyCurve at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/band_novelty.h>
#include <dsp_kernels/log_compression.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedBandNovelty::prepare_weights(std::span<const float> supplied) {
    const auto count=config_.weighting==NoveltyWeight::Hybrid?4u:1u;
    weights_.resize(count*bands_);
    if(count==1)return novelty_band_weights(weights_,config_.weighting,supplied);
    constexpr std::array kinds{NoveltyWeight::Flat,NoveltyWeight::Quadratic,NoveltyWeight::Linear,NoveltyWeight::InverseQuadratic};
    for(std::size_t i=0;i<kinds.size();++i)
        if(!novelty_band_weights(std::span(weights_).subspan(i*bands_,bands_),kinds[i]))return false;
    return true;
}
bool PreparedBandNovelty::configure(std::size_t frames,std::size_t bands,BandNoveltyConfig config,std::span<const float> supplied) {
    if(!frames || !bands || bands>46340 || frames>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/bands)return false;
    if(!std::isfinite(config.frame_rate) || config.frame_rate<1)return false;
    const double smoothing=.1*double(config.frame_rate);
    if(smoothing<1 || smoothing>double(std::numeric_limits<std::ptrdiff_t>::max()/2))return false;
    mean_size_=std::size_t(smoothing);mean_size_+=mean_size_%2;
    config_=config;maximum_frames_=frames;bands_=bands;
    if(!prepare_weights(supplied) || !average_.prepare(mean_size_))return false;
    row_.resize(bands);current_.resize(bands);previous_.resize(bands);
    derived_.resize((frames-1)*bands);mixed_.resize(frames-1);return true;
}
bool PreparedBandNovelty::prepare(std::size_t frames,std::size_t bands,BandNoveltyConfig config,std::span<const float> supplied) {
    std::size_t bound=0;if(!storage_bytes(frames,bands,config,supplied,bound))return false;
    PreparedBandNovelty next;
    if(!next.configure(frames,bands,config,supplied) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedBandNovelty::store_difference(std::size_t frame) noexcept {
    for(std::size_t band=0;band<bands_;++band)
        derived_[band*(maximum_frames_-1)+frame]=std::max(0.f,current_[band]-previous_[band]);
}
bool PreparedBandNovelty::derivatives(dsp_kernels::FloatMatrixSource source,std::size_t frames) noexcept {
    for(std::size_t i=0;i<frames;++i){
        if(!source.read(source.context,i,row_) || !dsp_kernels::logarithmic_compression(row_,current_,1000,1))return false;
        if(i)store_difference(i-1);
        current_.swap(previous_);
    }
    return true;
}
float PreparedBandNovelty::local_mean(std::span<const float> values,std::size_t position) const noexcept {
    const auto size=std::ptrdiff_t(values.size()),width=std::ptrdiff_t(mean_size_);
    auto first=std::ptrdiff_t(position)-width/2,last=std::ptrdiff_t(position)+width/2;
    if(first<0 && last>=size){first=0;last=size;}
    else{
        if(first<0){first=0;last=width;}
        if(last>=size){last=size;first=size-width;}
    }
    first=std::max(std::ptrdiff_t(0),first);last=std::min(size,last);
    if(first>=last)return 0;
    return std::accumulate(values.begin()+first,values.begin()+last,0.f)/float(last-first);
}
void PreparedBandNovelty::remove_local_mean(std::span<float> values) const noexcept {
    for(std::size_t i=0;i<values.size();++i)values[i]=std::max(0.f,values[i]-local_mean(values,i));
}
bool PreparedBandNovelty::process_band(std::size_t band,std::size_t frames) noexcept {
    auto values=std::span(derived_).subspan(band*(maximum_frames_-1),frames);
    remove_local_mean(values);
    if(config_.normalize_bands && !values.empty()){
        const float maximum=*std::max_element(values.begin(),values.end());
        if(maximum!=0)for(auto& value:values)value/=maximum;
    }
    average_.reset();return average_.process(values,values);
}
float PreparedBandNovelty::mix_frame(std::size_t frame) const noexcept {
    std::array<float,4> sums{};
    const auto curves=config_.weighting==NoveltyWeight::Hybrid?4u:1u;
    for(std::size_t band=0;band<bands_;++band)
        for(std::size_t curve=0;curve<curves;++curve)
            sums[curve]+=weights_[curve*bands_+band]*derived_[band*(maximum_frames_-1)+frame];
    float result=sums[0];
    for(std::size_t curve=1;curve<curves;++curve)result*=sums[curve];
    return result;
}
bool PreparedBandNovelty::mix(std::size_t frames) noexcept {
    for(std::size_t frame=0;frame<frames;++frame)mixed_[frame]=mix_frame(frame);
    average_.reset();
    const auto values=std::span(mixed_).first(frames);
    return average_.process(values,values);
}
bool PreparedBandNovelty::process(dsp_kernels::FloatMatrixSource source,std::size_t frames,std::span<float> output) noexcept {
    active_=0;
    if(!source.read || !frames || frames>maximum_frames_ || output.size()!=frames-1)return false;
    if(!derivatives(source,frames))return false;
    for(std::size_t band=0;band<bands_;++band)if(!process_band(band,frames-1))return false;
    if(!mix(frames-1))return false;
    std::copy_n(mixed_.begin(),output.size(),output.begin());active_=frames-1;return true;
}
std::span<const float> PreparedBandNovelty::band_novelty(std::size_t band) const noexcept {
    if(band>=bands_ || !active_)return {};
    return std::span(derived_).subspan(band*(maximum_frames_-1),active_);
}
}
