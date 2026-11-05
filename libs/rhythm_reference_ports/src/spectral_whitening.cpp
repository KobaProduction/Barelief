// SPDX-License-Identifier: GPL-3.0-or-later
// Port of aubio/src/spectral/awhitening.c at
// aubio/aubio@ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2015 Paul Brossier <piem@aubio.org>.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/spectral_whitening.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace rhythm_reference_ports {
bool PreparedSpectralWhitening::storage_bytes(std::size_t bins,SpectralWhiteningConfig c,std::size_t& bytes) noexcept {
    if(!bins || bins>SIZE_MAX/(3*sizeof(float)) || !std::isfinite(c.frame_period_s) || c.frame_period_s<=0)return false;
    if(!std::isfinite(c.relax_time_s) || c.relax_time_s<=0 || !std::isfinite(c.floor) || c.floor<=0)return false;
    bytes=3*bins*sizeof(float);return true;
}
bool PreparedSpectralWhitening::prepare(std::size_t bins,SpectralWhiteningConfig c) {
    std::size_t bytes=0;if(!storage_bytes(bins,c,bytes))return false;
    PreparedSpectralWhitening next;next.config_=c;
    next.peaks_.assign(bins,c.floor);next.next_.resize(bins);next.staging_.resize(bins);
    if(!next.set_relax_time(c.relax_time_s))return false;
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedSpectralWhitening::reset() noexcept {std::fill(peaks_.begin(),peaks_.end(),config_.floor);}
bool PreparedSpectralWhitening::set_relax_time(float seconds) noexcept {
    if(peaks_.empty() || !std::isfinite(seconds) || seconds<=0)return false;
    const float decay=std::pow(.001f,config_.frame_period_s/seconds);if(!std::isfinite(decay))return false;
    config_.relax_time_s=seconds;decay_=decay;return true;
}
bool PreparedSpectralWhitening::set_floor(float floor) noexcept {
    if(peaks_.empty() || !std::isfinite(floor) || floor<=0)return false;
    config_.floor=floor;return true;
}
bool PreparedSpectralWhitening::process(std::span<const float> input,std::span<float> output) noexcept {
    if(peaks_.empty() || input.size()!=peaks_.size() || output.size()!=input.size())return false;
    for(std::size_t i=0;i<input.size();++i){
        if(!std::isfinite(input[i]) || input[i]<0)return false;
        next_[i]=std::max(input[i],std::max(decay_*peaks_[i],config_.floor));
        staging_[i]=input[i]/next_[i];
    }
    peaks_.swap(next_);std::copy(staging_.begin(),staging_.end(),output.begin());return true;
}
}
