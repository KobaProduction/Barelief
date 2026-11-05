// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of OnsetDetectionGlobal at MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/global_onset.h>
#include <dsp_kernels/sequence_statistics.h>
#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace rhythm_reference_ports {
namespace {
bool nonnegative(std::span<const float> x) noexcept {
    return std::all_of(x.begin(),x.end(),[](float v){return std::isfinite(v) && v>=0;});
}
constexpr std::size_t half_window=8,tail=2*half_window;
}
bool PreparedInformationGain::prepare(std::size_t bins,std::size_t first,std::size_t stop) {
    std::size_t admitted=0;if(!storage_bytes(bins,first,stop,admitted))return false;
    if(!bins || first>=stop || stop>bins || stop-first>SIZE_MAX/11)return false;
    PreparedInformationGain next;next.bins_=bins;next.first_=first;next.count_=stop-first;
    next.history_.assign(11*next.count_,0);if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedInformationGain::reset() noexcept {
    std::fill(history_.begin(),history_.end(),0);next_=0;
}
float PreparedInformationGain::bin_change(std::size_t bin) const noexcept {
    float old=0,recent=0;
    for(std::size_t i=0;i<5;++i){
        const float weight=1.f-float(i)*.9f/5.f;
        const float old_weight=1.f-float(4-i)*.9f/5.f;
        old+=history_[((next_+i)%11)*count_+bin]*old_weight;
        recent+=history_[((next_+6+i)%11)*count_+bin]*weight;
    }
    if(old==0)old=1;
    if(recent==0)recent=std::numeric_limits<float>::epsilon();
    return std::max(std::log2(recent/old),0.f);
}
bool PreparedInformationGain::process(std::span<const float> magnitude,float& output) noexcept {
    if(!count_ || magnitude.size()!=bins_ || !nonnegative(magnitude))return false;
    std::copy_n(magnitude.begin()+std::ptrdiff_t(first_),count_,history_.begin()+std::ptrdiff_t(next_*count_));
    next_=(next_+1)%11;float value=0;
    for(std::size_t i=0;i<count_;++i)value+=bin_change(i);
    if(!std::isfinite(value))return false;
    output=value;return true;
}
bool PreparedBandBeatEmphasis::prepare(std::size_t frames,BandBeatEmphasisConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t admitted=0;if(!storage_bytes(frames,config,admitted,factory))return false;
    PreparedBandBeatEmphasis next;if(!next.configure(frames,config,factory))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedBandBeatEmphasis::configure(std::size_t frames,BandBeatEmphasisConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(!frames || !config.bands || frames>SIZE_MAX-tail || !std::isfinite(config.frame_rate) || config.frame_rate<=0)return false;
    const float period=std::round(5.944308390022676f*config.frame_rate);
    if(!std::isfinite(period) || period<32 || period>=float(PTRDIFF_MAX))return false;
    const float ray=std::round(43.f*512.f/period);
    if(ray<=0 || config.bands>SIZE_MAX/(frames+tail))return false;
    set_geometry(frames,config,std::size_t(period));
    if(!average_.prepare(tail+1) || !correlation_.prepare(frames+tail,dsp_kernels::CorrelationNormalization::Unbiased,factory))return false;
    allocate();
    tempo_weights(ray);return true;
}
void PreparedBandBeatEmphasis::set_geometry(std::size_t frames,BandBeatEmphasisConfig config,std::size_t period) noexcept {
    config_=config;maximum_frames_=frames;stride_=frames+tail;maximum_period_=period;
}
void PreparedBandBeatEmphasis::tempo_weights(float ray) noexcept {
    for(std::size_t i=0;i<maximum_period_;++i){
        const float tau=float(i+1);rayleigh_[i]=tau/(ray*ray)*std::exp(-.5f*tau*tau/(ray*ray));
    }
}
void PreparedBandBeatEmphasis::allocate() {
    rows_.resize(config_.bands*stride_);row_.resize(config_.bands);acf_.resize(maximum_period_);
    rayleigh_.resize(maximum_period_);weights_.resize(config_.bands);sorted_.resize(config_.bands);result_.resize(maximum_frames_);
}
bool PreparedBandBeatEmphasis::read(dsp_kernels::FloatMatrixSource source,std::size_t frames) noexcept {
    if(!source.read)return false;
    for(std::size_t frame=0;frame<frames;++frame){
        if(!source.read(source.context,frame,row_) || !nonnegative(row_))return false;
        for(std::size_t band=0;band<config_.bands;++band)rows_[band*stride_+frame]=row_[band];
    }
    return true;
}
bool PreparedBandBeatEmphasis::normalize(std::span<float> band,std::size_t frames) noexcept {
    float variance=0;
    if(!dsp_kernels::variance_float32(band.first(frames),variance))return false;
    const float deviation=std::sqrt(variance);
    if(deviation>0)for(std::size_t i=0;i<frames;++i)band[i]/=deviation;
    std::fill(band.begin()+std::ptrdiff_t(frames),band.end(),band[frames-1]);return true;
}
bool PreparedBandBeatEmphasis::threshold(std::span<float> band,std::size_t frames) noexcept {
    // The first sixteen outputs are discarded, fully flushing the FIR's old
    // state. Keep the original forward alignment and appended tail in ACF.
    average_.reset();
    for(std::size_t i=0;i<frames+tail;++i){
        float mean=0;if(!average_.process(band[i],mean))return false;
        if(i<tail)continue;
        const auto at=i-tail;band[at]=std::max(0.f,band[at]-mean);
        if(!std::isfinite(band[at]))return false;
    }
    return true;
}
float PreparedBandBeatEmphasis::comb_weight() const noexcept {
    float best=0;
    const auto stop=(maximum_period_-3)/4-1;
    for(std::size_t period=3;period<stop;++period){
        float sum=0;
        for(std::ptrdiff_t comb=1;comb<=4;++comb)
            for(auto region=1-comb;region<comb;++region)
                sum+=rayleigh_[period]*acf_[std::size_t(std::ptrdiff_t(period)*comb+region)]/float(2*comb-1);
        best=std::max(best,sum);
    }
    return best;
}
bool PreparedBandBeatEmphasis::weight(std::size_t band,std::size_t frames) noexcept {
    auto values=std::span(rows_).subspan(band*stride_,frames+tail);
    if(!normalize(values,frames) || !threshold(values,frames))return false;std::fill(acf_.begin(),acf_.end(),0);
    const auto lags=std::min(acf_.size(),values.size());
    if(!correlation_.process(values,std::span(acf_).first(lags)))return false;
    weights_[band]=comb_weight();return std::isfinite(weights_[band]);
}
float PreparedBandBeatEmphasis::select_weights() noexcept {
    const float maximum=*std::max_element(weights_.begin(),weights_.end());
    if(maximum>0)for(float& value:weights_)value/=maximum;
    std::copy(weights_.begin(),weights_.end(),sorted_.begin());std::sort(sorted_.begin(),sorted_.end());
    return sorted_[std::size_t(std::floor(float(config_.bands)*.6f))];
}
bool PreparedBandBeatEmphasis::combine(std::size_t frames,std::span<float> output) noexcept {
    const float threshold=select_weights();std::fill_n(result_.begin(),frames,0);
    for(std::size_t band=0;band<config_.bands;++band)
        if(weights_[band]>=threshold)
            for(std::size_t i=0;i<frames;++i)result_[i]+=rows_[band*stride_+i]*weights_[band];
    if(!nonnegative(std::span(result_).first(frames)))return false;
    std::copy_n(result_.begin(),frames,output.begin());return true;
}
bool PreparedBandBeatEmphasis::process(dsp_kernels::FloatMatrixSource source,std::size_t frames,std::span<float> output) noexcept {
    if(!maximum_frames_ || frames>maximum_frames_ || output.size()!=frames)return false;
    if(!frames){std::fill(weights_.begin(),weights_.end(),0);return true;}
    if(!read(source,frames))return false;
    for(std::size_t band=0;band<config_.bands;++band)if(!weight(band,frames))return false;
    return combine(frames,output);
}
}
