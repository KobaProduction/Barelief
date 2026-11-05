// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia PercivalBpmEstimator's OSS analysis recipe.
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/pulse_correlation.h>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
InterpolatedPeakConfig pulse_peak_config(std::size_t frames,float rate,float minimum,float maximum) noexcept {
    InterpolatedPeakConfig peak_config;
    peak_config.range=float(frames-1);peak_config.minimum=std::trunc(rate*60.f/maximum);
    peak_config.maximum=std::trunc(rate*60.f/minimum);peak_config.maximum_peaks=10;peak_config.ordering=PeakOrdering::Amplitude;
    return peak_config;
}
}
bool PreparedPulseCorrelation::configure(std::size_t frames,float rate,float minimum,float maximum,const dsp_kernels::FourierBackendFactory* factory) {
    if(frames<2 || frames>std::numeric_limits<std::size_t>::max()/4)return false;
    if(!std::isfinite(rate) || rate<=0 || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum<=0 || minimum>=maximum)return false;
    const auto length=std::bit_ceil(2*frames);
    const dsp_kernels::SpectralAutocorrelationConfig correlation_config{length,float(length),.5f,float(length)};
    const auto peak_config=pulse_peak_config(frames,rate,minimum,maximum);
    if(!correlation_.prepare(frames,correlation_config,factory) || !peaks_.prepare(frames,peak_config) || !scoring_.prepare(frames,10))return false;
    correlation_values_.resize(frames);enhanced_.resize(frames);candidate_lags_.resize(10);return true;
}
bool PreparedPulseCorrelation::prepare(std::size_t frames,float rate,float minimum,float maximum,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bound=0;if(!storage_bytes(frames,rate,minimum,maximum,bound,factory))return false;
    PreparedPulseCorrelation next;
    if(!next.configure(frames,rate,minimum,maximum,factory) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedPulseCorrelation::process(std::span<const float> onset,float& selected) noexcept {
    if(!correlation_.process(onset,correlation_values_) || !enhance_correlation_harmonics(correlation_values_,enhanced_))return false;
    if(!peaks_.process(enhanced_))return false;
    const auto peaks=peaks_.peaks();
    for(std::size_t i=0;i<peaks.size();++i)candidate_lags_[i]=peaks[i].position;
    return scoring_.process(onset,std::span(candidate_lags_).first(peaks.size()),selected);
}
}
