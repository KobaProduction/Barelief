// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
namespace rhythm_reference_ports {
// Source controls: delay seconds truncate; min-IOI seconds round away from zero.
// Wide counters retain precision independently from Float32 analytical data.
inline bool unsigned_sample_offset(float seconds,std::uint32_t rate,bool rounded,std::uint64_t& output) noexcept {
    if(!rate || !std::isfinite(seconds) || seconds<0)return false;
    const float product=seconds*float(rate),samples=rounded?std::round(product):product;
    if(!std::isfinite(samples) || samples<0 || samples>=float(UINT64_MAX))return false;
    output=std::uint64_t(samples);return true;
}
inline bool signed_sample_offset(float seconds,std::uint32_t rate,std::int32_t& output) noexcept {
    if(!rate || !std::isfinite(seconds))return false;
    const float samples=seconds*float(rate);
    if(!std::isfinite(samples) || samples<float(INT32_MIN) || samples>=float(INT32_MAX))return false;
    output=std::int32_t(samples);return true;
}
}
