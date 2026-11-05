// SPDX-License-Identifier: GPL-3.0-or-later
// aubio onset.c/awhitening.c/cvec_logmag composition at
// ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2015 Paul Brossier; GPL3 or later, no warranty, see ../COPYING.
#include <rhythm_reference_ports/onset_magnitude.h>
#include <dsp_kernels/log_compression.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace rhythm_reference_ports {
bool PreparedOnsetMagnitude::storage_bytes(std::size_t bins,OnsetMagnitudeConfig config,std::size_t& bytes) noexcept {
    return std::isfinite(config.compression) && config.compression>=0 &&
        PreparedSpectralWhitening::storage_bytes(bins,config.whitening,bytes);
}
bool PreparedOnsetMagnitude::prepare(std::size_t bins,OnsetMagnitudeConfig config) {
    std::size_t bytes=0;if(!storage_bytes(bins,config,bytes))return false;
    PreparedOnsetMagnitude next;if(!next.whitening_.prepare(bins,config.whitening) || next.workspace_bytes()>bytes)return false;
    next.bins_=bins;next.config_=config;*this=std::move(next);return true;
}
bool PreparedOnsetMagnitude::set_whitening(bool enabled) noexcept {
    if(!bins_)return false;
    config_.whiten=enabled;return true;
}
bool PreparedOnsetMagnitude::set_compression(float value) noexcept {
    if(!bins_ || !std::isfinite(value) || value<0)return false;
    config_.compression=value;return true;
}
bool PreparedOnsetMagnitude::process(std::span<float> values) noexcept {
    if(values.size()!=bins_ || !bins_ || !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    if(config_.whiten && !whitening_.process(values,values))return false;
    return config_.compression==0 || dsp_kernels::logarithmic_magnitude(values,values,config_.compression);
}
}
