// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/loop_length_confidence.h>
#include <rhythm_reference_ports/spectral_band_ratios.h>
#include <rhythm_reference_ports/beat_band_salience.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
using namespace dsp_kernels::prepared_storage;
namespace {
bool band_storage(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept {
    if(!bins || bins>INT_MAX || !std::isfinite(rate) || rate<=0 || rate/2<=0 || borders.empty())return false;
    float previous=0;
    for(float value:borders){if(!std::isfinite(value) || value<previous)return false;previous=value;}
    const auto bands=borders.size()-1;std::size_t weights=0;
    bytes=0;if(!bands)return true;
    return MelFilterBank::matrix_storage_bytes(bins,bands,bytes) && mul_bytes(bins,bands,weights) &&
        add_array(bytes,weights,sizeof(float)) && add_array(bytes,bands,sizeof(double)+sizeof(FilterBandSupport));
}
}
bool PreparedLoopLengthConfidence::storage_bytes(float rate,std::size_t& bytes) noexcept {
    bytes=0;return std::isfinite(rate) && rate>=1 && rate<=std::nextafter(float(INT_MAX),0.f);
}
bool PreparedFrequencyBandEnergy::storage_bytes(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept {
    return bins>=2 && borders.size()>=2 && std::adjacent_find(borders.begin(),borders.end())==borders.end() && band_storage(bins,rate,borders,bytes);
}
bool PreparedSpectralBandRatios::storage_bytes(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept {
    std::size_t descriptor=0;
    return band_storage(bins,rate,borders,bytes) && PreparedSpectralDescriptor::storage_bytes(bins,descriptor) && add_bytes(bytes,descriptor) && add_array(bytes,borders.size()-1,sizeof(float));
}
bool PreparedBeatBandSalience::storage_bytes(std::size_t beats,std::size_t bands,std::size_t window,std::size_t& bytes) noexcept {
    std::size_t cells=0;bytes=0;
    return beats && bands && window && beats<=std::size_t(PTRDIFF_MAX)/2 && mul_bytes(beats,bands,cells) &&
        add_array(bytes,cells,sizeof(float)) && add_array(bytes,bands,sizeof(float)) && add_array(bytes,std::min(beats,window),sizeof(float)) && add_array(bytes,beats,2*sizeof(float));
}
std::size_t PreparedBeatBandSalience::workspace_bytes() const noexcept {
    return (values_.capacity()+row_.capacity()+scratch_.capacity()+tick_thresholds_.capacity()+band_thresholds_.capacity())*sizeof(float);
}
}
