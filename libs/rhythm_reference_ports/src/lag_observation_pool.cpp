// SPDX-License-Identifier: AGPL-3.0-or-later
// Source-compatible Pool row semantics for Essentia RhythmExtractor at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/lag_observation_pool.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedLagObservationPool::prepare(std::size_t rows,std::size_t values,std::size_t flat) {
    std::size_t admitted=0;if(!storage_bytes(rows,values,flat,admitted))return false;
    if(!rows || !values || !flat)return false;
    PreparedLagObservationPool next;next.maximum_flat_=flat;
    next.values_.resize(values);next.rows_.resize(rows);if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedLagObservationPool::reset() noexcept {
    value_count_=0;row_count_=0;flat_count_=0;active_start_=0;active_count_=0;
}
bool PreparedLagObservationPool::push(std::span<const float> values) noexcept {
    if(!maximum_flat_)return false;
    if(values.empty()){active_start_=value_count_;active_count_=0;return true;}
    if(values.size()<active_count_ || row_count_==rows_.size() || values.size()>maximum_flat_-flat_count_ ||
       values.size()-active_count_>values_.size()-value_count_)return false;
    if(!std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}) ||
       !std::equal(values.begin(),values.begin()+std::ptrdiff_t(active_count_),values_.begin()+std::ptrdiff_t(active_start_)))return false;
    const auto suffix=values.subspan(active_count_);
    std::copy(suffix.begin(),suffix.end(),values_.begin()+std::ptrdiff_t(value_count_));
    value_count_+=suffix.size();flat_count_+=values.size();
    rows_[row_count_++]={active_start_,flat_count_};active_count_=values.size();return true;
}
bool PreparedLagObservationPool::read(const void* context,std::size_t at,float& output) noexcept {
    const auto& self=*static_cast<const PreparedLagObservationPool*>(context);
    if(at>=self.flat_count_)return false;
    const auto begin=self.rows_.begin(),end=begin+std::ptrdiff_t(self.row_count_);
    const auto row=std::upper_bound(begin,end,at,[](std::size_t value,const Row& item){return value<item.end;});
    if(row==end)return false;
    const auto offset=row==begin?0:(row-1)->end;
    output=self.values_[row->start+at-offset];return true;
}
}
