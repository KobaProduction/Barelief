// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia experimental Beatogram/Meter at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/correlation_meter.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool power_harmonic(std::size_t left,std::size_t right) noexcept {
    if(left<2 || right<2)return false;
    if(left<right)std::swap(left,right);
    if(left%right)return false;
    if(std::has_single_bit(left/right))return true;
    const float power=std::log(float(left))/std::log(float(right));
    return power-std::trunc(power)==0;
}
std::size_t peak_lag(std::span<const float> values) noexcept {
    return std::size_t(std::max_element(values.begin()+2,values.end())-values.begin());
}
}
bool PreparedCorrelationMeter::prepare(std::size_t beats,std::size_t bands,const dsp_kernels::FourierBackendFactory* factory) {
    if(beats<3 || beats>std::size_t(std::numeric_limits<int>::max()) || !bands)return false;
    std::size_t bound=0;if(!storage_bytes(beats,bands,bound,factory))return false;
    PreparedCorrelationMeter next;
    if(!next.correlation_.prepare(beats,dsp_kernels::CorrelationNormalization::Raw,factory))return false;
    next.allocate(beats,bands);if(next.workspace_bytes()>bound)return false;*this=std::move(next);return true;
}
void PreparedCorrelationMeter::allocate(std::size_t beats,std::size_t bands) {
    maximum_beats_=beats;maximum_bands_=bands;
    row_.resize(beats);acf_.resize(beats);sum_.resize(beats);histogram_.resize(beats);
    band_maxima_.resize(bands);band_lags_.resize(bands);
}
bool PreparedCorrelationMeter::accumulate(dsp_kernels::FloatMatrixSource source,std::size_t bands,std::size_t beats) noexcept {
    auto row=std::span(row_).first(beats),acf=std::span(acf_).first(beats);
    std::fill_n(sum_.begin(),beats,0.f);
    for(std::size_t band=0;band<bands;++band){
        if(!source.read(source.context,band,row) || !correlation_.process(row,acf))return false;
        band_lags_[band]=peak_lag(acf);band_maxima_[band]=acf[band_lags_[band]];
        for(std::size_t beat=0;beat<beats;++beat)sum_[beat]+=acf[beat];
    }
    return std::all_of(sum_.begin(),sum_.begin()+std::ptrdiff_t(beats),[](float x){return std::isfinite(x);});
}
bool PreparedCorrelationMeter::harmonic_histogram(std::size_t beats) noexcept {
    for(std::size_t i=0;i<beats;++i){
        float value=0;std::size_t count=0;
        for(std::size_t j=0;j<beats;++j)if(power_harmonic(i,j)){value+=sum_[j];++count;}
        histogram_[i]=count?value/float(count):value;
        if(!std::isfinite(histogram_[i]))return false;
    }
    return true;
}
bool PreparedCorrelationMeter::process(dsp_kernels::FloatMatrixSource source,std::size_t bands,std::size_t beats,float& meter) noexcept {
    ready_=false;beats_=0;bands_=0;
    if(beats<3 || beats>maximum_beats_ || !bands || bands>maximum_bands_ || !source.read)return false;
    if(!accumulate(source,bands,beats) || !harmonic_histogram(beats))return false;
    meter=float(peak_lag(std::span(sum_).first(beats)));beats_=beats;bands_=bands;ready_=true;return true;
}
std::span<const float> PreparedCorrelationMeter::summed_correlation() const noexcept {
    return ready_?std::span<const float>(sum_).first(beats_):std::span<const float>{};
}
std::span<const float> PreparedCorrelationMeter::harmonic_histogram() const noexcept {
    return ready_?std::span<const float>(histogram_).first(beats_):std::span<const float>{};
}
std::span<const std::size_t> PreparedCorrelationMeter::band_peak_lags() const noexcept {
    return ready_?std::span<const std::size_t>(band_lags_).first(bands_):std::span<const std::size_t>{};
}
std::span<const float> PreparedCorrelationMeter::band_peak_values() const noexcept {
    return ready_?std::span<const float>(band_maxima_).first(bands_):std::span<const float>{};
}
}
