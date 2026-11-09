// Adapted from madmom features/beats.py CRFBeatDetectionProcessor.process at
// revision 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/interval_sequence_search.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
namespace dsp_kernels {
namespace {
std::size_t bounded_factor_intervals(float dominant,std::span<const float> factors,std::size_t low,std::size_t high,std::span<std::size_t> output) noexcept {
    std::size_t result=0;
    for(float factor:factors){
        const float value=std::trunc(dominant*factor);
        if(value<0 || value>=float(SIZE_MAX))continue;
        const auto interval=std::size_t(value);
        if(interval>=low && interval<=high)output[result++]=interval;
    }
    return result;
}
}
bool factor_beat_intervals(float dominant,std::span<const float> factors,std::size_t low,std::size_t high,std::span<std::size_t> output,std::size_t& count) noexcept {
    if(!std::isfinite(dominant) || dominant<=0 || !low || low>high || output.size()<factors.size())return false;
    for(float factor:factors)if(!std::isfinite(factor) || !std::isfinite(dominant*factor))return false;
    count=bounded_factor_intervals(dominant,factors,low,high,output);
    return true;
}
void PreparedIntervalSequenceSearch::allocate_results(std::size_t frames,std::size_t low,std::size_t high,std::size_t candidates) {
    minimum_=low;maximum_=high;
    intervals_.resize(candidates);scores_.resize(candidates);positions_.resize(frames/low);
}
bool PreparedIntervalSequenceSearch::prepare(std::size_t frames,std::size_t low,std::size_t high,std::size_t candidates,float sigma) {
    std::size_t bound=0;if(!storage_bytes(frames,low,high,candidates,sigma,bound))return false;
    if(!candidates)return false;
    PreparedIntervalSequenceSearch next;if(!next.decoder_.prepare(frames,low,high,sigma))return false;
    next.allocate_results(frames,low,high,candidates);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedIntervalSequenceSearch::trial(std::span<const float> input,std::size_t trial_index,float& best) noexcept {
    scores_[trial_index]=-std::numeric_limits<float>::infinity();
    if(!decoder_.process(input,intervals_[trial_index]) || decoder_.positions().empty())return false;
    const float score=decoder_.log_probability()/float(decoder_.positions().size());scores_[trial_index]=score;
    if(score>best){
        best=score;count_=decoder_.positions().size();
        std::copy(decoder_.positions().begin(),decoder_.positions().end(),positions_.begin());
    }
    return true;
}
void PreparedIntervalSequenceSearch::load_candidates(std::span<const std::size_t> candidates) noexcept {
    std::fill(scores_.begin(),scores_.end(),-std::numeric_limits<float>::infinity());
    std::copy(candidates.begin(),candidates.end(),intervals_.begin());
    trials_=candidates.size();
    std::sort(intervals_.begin(),intervals_.begin()+std::ptrdiff_t(trials_),std::greater<>{});
}
bool PreparedIntervalSequenceSearch::process(std::span<const float> input,std::span<const std::size_t> candidates) noexcept {
    clear_result();
    if(!minimum_ || candidates.empty() || candidates.size()>intervals_.size())return fail("invalid_candidates");
    if(!std::all_of(candidates.begin(),candidates.end(),[this](auto interval){return interval>=minimum_ && interval<=maximum_;}))return fail("invalid_candidates");
    load_candidates(candidates);
    float best=-std::numeric_limits<float>::infinity();
    for(std::size_t i=0;i<trials_;++i){
        if(!trial(input,i,best) && decoder_.outcome()!=SequenceDecodeOutcome::NoPath)
            return fail(decoder_.error_reason()?decoder_.error_reason():"empty_successful_decoder");
    }
    outcome_=count_?SequenceDecodeOutcome::Path:SequenceDecodeOutcome::NoPath;error_reason_=nullptr;
    return count_>0;
}
void PreparedIntervalSequenceSearch::clear_result() noexcept {
    count_=0;trials_=0;outcome_=SequenceDecodeOutcome::Error;error_reason_="not_processed";
}
bool PreparedIntervalSequenceSearch::fail(const char* reason) noexcept {
    count_=0;trials_=0;outcome_=SequenceDecodeOutcome::Error;error_reason_=reason;return false;
}
}


namespace dsp_kernels {
bool PreparedIntervalSequenceSearch::storage_bytes(std::size_t frames,std::size_t low,std::size_t high,std::size_t candidates,float sigma,std::size_t& out) noexcept {
    std::size_t bytes=0;
    if(!candidates || !PreparedIntervalSequenceDecoder::storage_bytes(frames,low,high,sigma,bytes) ||
       !prepared_storage::add_array(bytes,candidates,sizeof(std::size_t)+sizeof(float)) || !prepared_storage::add_array(bytes,frames/low,sizeof(std::size_t)))return false;
    out=bytes;return true;
}
}
