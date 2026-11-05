// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of ERBBands at MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <dsp_kernels/filter_projection.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <limits>
#include <utility>
namespace rhythm_reference_ports {
namespace {
using Complex=std::complex<float>;
constexpr float pi=std::numbers::pi_v<float>,ear_q=9.26449f,min_bw=24.7f;
struct BandDesign {
    float time,decay,angle,frequency,bandwidth;
    static std::array<float,2> roots() noexcept {
        return {float(std::sqrt(3.+std::pow(2.,1.5))),float(std::sqrt(3.-std::pow(2.,1.5)))};
    }
    std::array<float,4> zeros() const noexcept {
        const float sine=std::sin(2.f*frequency*pi*time),cosine=std::cos(2.f*frequency*pi*time);
        const auto root=roots();const float exponential=std::exp(bandwidth*time);
        const float gt_cos=2.f*time*cosine/exponential,gt_sin=time*sine/exponential;
        const std::array<float,4> numerator{-(gt_cos+2.f*root[0]*gt_sin)/2.f,-(gt_cos-2.f*root[0]*gt_sin)/2.f,
                                            -(gt_cos+2.f*root[1]*gt_sin)/2.f,-(gt_cos-2.f*root[1]*gt_sin)/2.f};
        return {-numerator[0]/time,-numerator[1]/time,-numerator[2]/time,-numerator[3]/time};
    }
    float gain() const noexcept {
        const float sine=std::sin(2.f*frequency*pi*time),cosine=std::cos(2.f*frequency*pi*time);
        const auto root=roots();const Complex unit{0,1};
        const auto phase=std::exp(4.f*unit*frequency*pi*time),g1=-2.f*phase*time;
        const auto g2=2.f*std::exp(-bandwidth*time+2.f*unit*frequency*pi*time)*time;
        const auto denominator=-2.f/std::exp(2.f*bandwidth*time)-2.f*phase+2.f*(1.f+phase)/std::exp(bandwidth*time);
        return std::abs((g1+g2*(cosine-root[1]*sine))*(g1+g2*(cosine+root[1]*sine))*
                        (g1+g2*(cosine-root[0]*sine))*(g1+g2*(cosine+root[0]*sine))/std::pow(denominator,4.f));
    }
};
BandDesign source_design(float frequency,ErbFilterBankConfig config) noexcept {
    const float erb=config.width*std::pow(std::pow(frequency/ear_q,1.f)+std::pow(min_bw,1.f),1.f);
    const float bandwidth=float(1.019f*2*std::numbers::pi*erb);
    const float decay=std::exp(-bandwidth/config.sample_rate);
    const float angle=float(2.f*std::numbers::pi*frequency/config.sample_rate);
    return {float(1./config.sample_rate),decay,angle,frequency,bandwidth};
}
bool valid(ErbFilterBankConfig c) noexcept {
    return c.bands && std::isfinite(c.sample_rate) && c.sample_rate>0 &&
        std::isfinite(c.low_hz) && c.low_hz>=0 && std::isfinite(c.high_hz) &&
        c.high_hz>c.low_hz && c.high_hz<=c.sample_rate*.5f && std::isfinite(c.width) && c.width>0;
}
void band_centers(std::span<float> centers,ErbFilterBankConfig c) noexcept {
    const float bw=ear_q*min_bw,inverse=float(1./double(c.bands));
    const float difference=-std::log(c.high_hz+bw)+std::log(c.low_hz+bw);
    for(std::size_t i=1;i<=c.bands;++i)centers[c.bands-i]=-bw+std::exp(float(i)*difference*inverse)*(c.high_hz+bw);
}
bool band_weights(std::span<float> weights,const std::array<float,4>& zeros,Complex pole,double scale) noexcept {
    for(std::size_t bin=0;bin<weights.size();++bin){
        const auto unit=std::exp(Complex(0,1)*2.f*pi*float(bin)/float(2*(weights.size()-1)));
        double weight=scale;
        for(float zero:zeros)weight*=std::abs(unit-zero);
        weight*=std::pow(std::abs((pole-unit)*(pole-unit)),-4.f);
        if(!std::isfinite(weight) || weight>double(std::numeric_limits<float>::max()))return false;
        weights[bin]=float(weight);
    }
    return true;
}
}
bool PreparedErbFilterBank::prepare(std::size_t bins,ErbFilterBankConfig c) {
    std::size_t admitted=0;if(!storage_bytes(bins,c,admitted))return false;
    PreparedErbFilterBank next;if(!next.configure(bins,c))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedErbFilterBank::configure(std::size_t bins,ErbFilterBankConfig c) {
    if(bins<2 || !valid(c) || c.bands>SIZE_MAX/bins)return false;
    config_=c;bins_=bins;weights_.resize(c.bands*bins);centers_.resize(c.bands);staging_.resize(c.bands);
    band_centers(centers_,c);
    for(std::size_t band=0;band<c.bands;++band)if(!build_band(band))return false;
    return true;
}
bool PreparedErbFilterBank::build_band(std::size_t band) {
    const auto design=source_design(centers_[band],config_);
    const auto zeros=design.zeros();const auto pole=design.decay*std::exp(Complex(0,1)*design.angle);
    // Upstream pow(Float32,int) promotes to Double; the complete coefficient
    // product is narrowed once. This mixed cold recipe keeps Float32 storage
    // and the existing Float32 processing projection.
    const float gain=design.gain();const double scale=std::pow(design.time,4)/gain;
    if(!std::isfinite(gain) || gain<=0 || !std::isfinite(scale))return false;
    return band_weights(std::span(weights_).subspan(band*bins_,bins_),zeros,pole,scale);
}
bool PreparedErbFilterBank::process(std::span<const float> input,std::span<float> output) const noexcept {
    if(!bins_ || input.size()!=bins_ || output.size()!=staging_.size())return false;
    for(std::size_t band=0;band<staging_.size();++band)
        if(!dsp_kernels::project_filter_weights(input,std::span(weights_).subspan(band*bins_,bins_),staging_[band],{false,false}))return false;
    std::copy(staging_.begin(),staging_.end(),output.begin());return true;
}
}
