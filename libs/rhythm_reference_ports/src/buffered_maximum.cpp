// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia MaxFilter at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/buffered_maximum.h>
#include <algorithm>
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
#include <functional>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool partial_overlap(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || input.data()==output.data())return false;
    const std::less<const float*> before;
    return before(input.data(),output.data()+output.size()) && before(output.data(),input.data()+input.size());
}
}
bool PreparedBufferedMaximum::prepare(std::size_t width,bool causal) {
    std::size_t bytes=0;if(!storage_bytes(width,bytes))return false;
    PreparedBufferedMaximum next;next.width_=width;next.causal_=causal;next.buffer_.resize(width);
    if(next.workspace_bytes()>bytes)return false;
    next.reset(BufferedMaximumReset::ConfiguredStart);*this=std::move(next);return true;
}
bool PreparedBufferedMaximum::storage_bytes(std::size_t width,std::size_t& bytes) noexcept {
    return width>=2 && dsp_kernels::prepared_storage::mul_bytes(width,sizeof(float),bytes);
}
void PreparedBufferedMaximum::reset(BufferedMaximumReset mode) noexcept {
    filled_=false;seeded_=false;current_maximum_=0;
    index_=mode==BufferedMaximumReset::ConfiguredStart && !causal_?width_/2:0;
}
void PreparedBufferedMaximum::seed(float value) noexcept {
    current_maximum_=value;
    if(!seeded_)std::fill(buffer_.begin(),buffer_.end(),value);
    seeded_=true;
}
std::size_t PreparedBufferedMaximum::fill(std::span<const float> input,std::span<float> output) noexcept {
    if(filled_)return 0;
    // Source: if(_bufferFillIdx == _causal ? 0 : _halfWidth).
    if(!seeded_ || (index_==std::size_t(causal_)?false:width_/2!=0))seed(input.front());
    const auto count=std::min(input.size(),width_-index_);
    for(std::size_t i=0;i<count;++i){
        const float value=input[i];buffer_[index_++]=value;
        current_maximum_=std::max(value,current_maximum_);output[i]=current_maximum_;
    }
    filled_=index_==width_;return count;
}
bool PreparedBufferedMaximum::process(std::span<const float> input,std::span<float> output) noexcept {
    if(!width_ || input.empty() || input.size()!=output.size() || partial_overlap(input,output) ||
       !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    const auto read=fill(input,output);
    for(std::size_t i=read;i<input.size();++i){
        index_%=width_;buffer_[index_++]=input[i];
        output[i]=*std::max_element(buffer_.begin(),buffer_.end());
    }
    return true;
}
}
