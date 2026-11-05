// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Koba Production. GPL version 3 or later; no warranty.
// Shared storage for the independently decomposed source trackers.
#include <rhythm_reference_ports/scalar_history.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
bool ScalarHistory::prepare(std::size_t count) {
    std::size_t bytes=0;if(!storage_bytes(count,bytes))return false;
    std::vector<float> next(2*count,0);if(next.capacity()>bytes/sizeof(float))return false;
    values_=std::move(next);count_=count;head_=0;return true;
}
bool ScalarHistory::storage_bytes(std::size_t count,std::size_t& bytes) noexcept {
    return count && dsp_kernels::prepared_storage::mul_bytes(count,2*sizeof(float),bytes);
}
void ScalarHistory::set(std::size_t index,float value) noexcept {
    if(index>=count_)return;
    const auto at=(head_+index)%count_;values_[at]=value;values_[at+count_]=value;
}
void ScalarHistory::push(float value) noexcept {
    if(!count_)return;
    values_[head_]=value;values_[head_+count_]=value;head_=(head_+1)%count_;
}
bool ScalarHistory::reset_length(std::size_t count,float initial_value) noexcept {
    if(!count || count>values_.size()/2)return false;
    count_=count;head_=0;std::fill_n(values_.begin(),2*count,initial_value);return true;
}
void ScalarHistory::clear() noexcept {std::fill(values_.begin(),values_.end(),0.f);head_=0;}
}
