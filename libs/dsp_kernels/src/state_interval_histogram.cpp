// Adapted from madmom features/tempo.py DBNTempoHistogramProcessor at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/state_interval_histogram.h>
#include <algorithm>
#include <utility>
namespace dsp_kernels {
bool PreparedStateIntervalHistogram::prepare(std::uint32_t low,std::uint32_t high,std::size_t frames) {
    std::size_t bound=0;if(!storage_bytes(low,high,frames,bound))return false;
    if(!low || low>high || !frames)return false;
    PreparedStateIntervalHistogram next;next.minimum_=low;next.maximum_=high;
    next.counts_.resize(std::size_t(high-low)+1);next.history_.resize(frames);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedStateIntervalHistogram::reset() noexcept {
    std::fill(counts_.begin(),counts_.end(),0);std::fill(history_.begin(),history_.end(),0);
    cursor_=0;filled_=0;ready_=false;streaming_=false;
}
bool PreparedStateIntervalHistogram::valid(std::span<const std::uint32_t> path,std::span<const std::uint32_t> intervals) const noexcept {
    if(!minimum_)return false;
    for(auto state:path)if(state>=intervals.size() || intervals[state]<minimum_ || intervals[state]>maximum_)return false;
    return true;
}
bool PreparedStateIntervalHistogram::offline(std::span<const std::uint32_t> path,std::span<const std::uint32_t> intervals) noexcept {
    ready_=false;if(!valid(path,intervals))return false;reset();
    for(auto state:path)++counts_[intervals[state]-minimum_];
    ready_=true;return true;
}
void PreparedStateIntervalHistogram::append(std::uint32_t interval) noexcept {
    if(filled_==history_.size())--counts_[history_[cursor_]-minimum_];else ++filled_;
    history_[cursor_]=interval;++counts_[interval-minimum_];
    if(++cursor_==history_.size())cursor_=0;
}
bool PreparedStateIntervalHistogram::online(std::span<const std::uint32_t> path,std::span<const std::uint32_t> intervals,bool reset_state) noexcept {
    ready_=false;if(!valid(path,intervals))return false;
    if(reset_state || !streaming_)reset();
    for(auto state:path)append(intervals[state]);
    streaming_=true;
    ready_=true;return true;
}
}


namespace dsp_kernels {
bool PreparedStateIntervalHistogram::storage_bytes(std::uint32_t low,std::uint32_t high,std::size_t history,std::size_t& out) noexcept {
    std::size_t bytes=0;
    if(!low || low>high || !history || !prepared_storage::add_array(bytes,std::size_t(high-low)+1,sizeof(std::uint64_t)) ||
       !prepared_storage::add_array(bytes,history,sizeof(std::uint32_t)))return false;
    out=bytes;return true;
}
}
