// SPDX-License-Identifier: AGPL-3.0-or-later
// Essentia MovingAverage composition at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/causal_average.h>
#include <array>
#include <algorithm>
#include <utility>
#include <vector>
#include <dsp_kernels/prepared_storage.h>

namespace rhythm_reference_ports {
bool PreparedCausalAverage::prepare(std::size_t length) {
    std::size_t bytes=0;if(!storage_bytes(length,bytes))return false;
    const std::vector<float> numerator(length,1.f/float(length));
    constexpr std::array denominator{1.f};
    PreparedCausalAverage next;
    if(!next.filter_.prepare(numerator,denominator))return false;
    next.coefficients_=numerator;if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedCausalAverage::storage_bytes(std::size_t length,std::size_t& output) noexcept {
    std::size_t bytes=0;
    if(!length || !dsp_kernels::PreparedRecursiveFilter::storage_bytes(length,1,{},bytes) ||
       !dsp_kernels::prepared_storage::add_array(bytes,length,2*sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedCausalAverage::set_length(std::size_t length) noexcept {
    if(!length || length>coefficients_.size())return false;
    std::fill_n(coefficients_.begin(),length,1.f/float(length));
    constexpr std::array denominator{1.f};
    return filter_.set_coefficients(std::span(coefficients_).first(length),denominator,false);
}
void PreparedCausalAverage::reset() noexcept {
    filter_.reset();
}
bool PreparedCausalAverage::process(float input,float& output) noexcept {
    return filter_.process(input,output);
}
bool PreparedCausalAverage::process(std::span<const float> input,std::span<float> output) noexcept {
    return filter_.process(input,output);
}
}
