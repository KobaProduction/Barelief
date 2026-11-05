// Adapted from madmom features/beats_crf.pyx at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/interval_sequence_decoder.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
namespace dsp_kernels {
namespace {
constexpr float unreachable=-std::numeric_limits<float>::infinity();
float logarithm(float value) noexcept {return value>0?std::log(value):unreachable;}
}
bool PreparedIntervalSequenceDecoder::prepare(std::size_t frames,std::size_t low,std::size_t high,float sigma) {
    std::size_t bound=0;if(!storage_bytes(frames,low,high,sigma,bound))return false;
    if(!frames || !low || low>high || high>frames || high>SIZE_MAX/2 || !std::isfinite(sigma) || sigma<=0)return false;
    if(frames/low-1>SIZE_MAX/frames)return false;
    PreparedIntervalSequenceDecoder next;next.maximum_frames_=frames;next.minimum_interval_=low;next.maximum_interval_=high;next.sigma_=sigma;
    next.allocate();if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedIntervalSequenceDecoder::allocate() {
    transition_.resize(2*maximum_interval_);log_transition_.resize(transition_.size());
    normalization_.resize(maximum_frames_);activation_.resize(maximum_frames_);
    previous_.resize(maximum_frames_);current_.resize(maximum_frames_);
    backlinks_.resize((maximum_frames_/minimum_interval_-1)*maximum_frames_);positions_.resize(maximum_frames_/minimum_interval_);
    probability_=unreachable;
}
bool PreparedIntervalSequenceDecoder::distribution(std::size_t interval) noexcept {
    transition_count_=2*interval;const float mean=std::log2(float(interval));float sum=0;
    for(std::size_t i=0;i<transition_count_;++i){
        const float z=(std::log2(i?float(i):.000001f)-mean)/sigma_;
        transition_[i]=std::exp(-.5f*z*z)/(sigma_*std::sqrt(2.f*std::numbers::pi_v<float>));sum+=transition_[i];
    }
    if(!std::isfinite(sum) || sum<=0)return false;
    for(std::size_t i=0;i<transition_count_;++i){
        transition_[i]/=sum;log_transition_[i]=logarithm(transition_[i]);
    }
    return true;
}
float PreparedIntervalSequenceDecoder::normalizer(std::span<const float> input,std::size_t frame) const noexcept {
    float value=0;
    for(std::size_t j=0;j<std::min(transition_count_,input.size()-frame);++j)value+=input[frame+j]*transition_[j];
    return value;
}
bool PreparedIntervalSequenceDecoder::load(std::span<const float> input,std::size_t interval) noexcept {
    for(std::size_t i=0;i<input.size();++i){
        const float normalization=normalizer(input,i);if(!std::isfinite(normalization))return false;
        normalization_[i]=logarithm(normalization);activation_[i]=logarithm(input[i]);
        previous_[i]=(i<interval?0.f:unreachable)+activation_[i]+normalization_[i];
    }
    return true;
}
float PreparedIntervalSequenceDecoder::predecessor(std::size_t state,std::size_t step) noexcept {
    float best=unreachable;auto& link=backlinks_[step*maximum_frames_+state];link=SIZE_MAX;
    // Source range excludes j==state, and includes the zero-delay entry.
    for(std::size_t j=0;j<std::min(state,transition_count_);++j){
        const float score=previous_[state-j]+log_transition_[j];
        if(score>best){best=score;link=state-j;}
    }
    return best;
}
void PreparedIntervalSequenceDecoder::advance(std::size_t frames,std::size_t step) noexcept {
    for(std::size_t state=0;state<frames;++state)
        current_[state]=predecessor(state,step)+activation_[state]+normalization_[state];
    previous_.swap(current_);
}
std::size_t PreparedIntervalSequenceDecoder::final_state(std::size_t frames) noexcept {
    std::size_t state=SIZE_MAX;float best=unreachable;
    for(std::size_t i=0;i<frames;++i){
        const float value=previous_[i]-normalization_[i];
        if(value>best){best=value;state=i;}
    }
    probability_=best;return std::isfinite(best)?state:SIZE_MAX;
}
bool PreparedIntervalSequenceDecoder::trace(std::size_t frames,std::size_t steps) noexcept {
    auto state=final_state(frames);
    if(state==SIZE_MAX)return probability_==unreachable?no_path():fail("nonfinite_terminal");
    positions_[steps-1]=state;
    for(std::size_t i=steps-1;i>0;--i){
        state=backlinks_[(i-1)*maximum_frames_+state];
        if(state>=frames)return fail("inconsistent_backtrace");
        positions_[i-1]=state;
    }
    count_=steps;outcome_=SequenceDecodeOutcome::Path;error_reason_=nullptr;return true;
}
void PreparedIntervalSequenceDecoder::clear_result() noexcept {
    count_=0;transition_count_=0;probability_=unreachable;
    outcome_=SequenceDecodeOutcome::Error;error_reason_="not_processed";
}
bool PreparedIntervalSequenceDecoder::no_path() noexcept {
    count_=0;probability_=unreachable;outcome_=SequenceDecodeOutcome::NoPath;error_reason_=nullptr;return false;
}
bool PreparedIntervalSequenceDecoder::fail(const char* reason) noexcept {
    count_=0;probability_=unreachable;outcome_=SequenceDecodeOutcome::Error;error_reason_=reason;return false;
}
bool PreparedIntervalSequenceDecoder::process(std::span<const float> input,std::size_t interval) noexcept {
    clear_result();
    if(!maximum_frames_ || input.size()>maximum_frames_ || interval<minimum_interval_ || interval>maximum_interval_)return fail("invalid_input");
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;}))return fail("invalid_activation");
    if(interval>input.size())return no_path();
    if(!distribution(interval) || !load(input,interval))return fail("nonfinite_distribution_or_normalizer");
    const auto steps=input.size()/interval;
    for(std::size_t step=0;step+1<steps;++step)advance(input.size(),step);
    return trace(input.size(),steps);
}
}


namespace dsp_kernels {
bool PreparedIntervalSequenceDecoder::storage_bytes(std::size_t frames,std::size_t low,std::size_t high,float sigma,std::size_t& out) noexcept {
    if(!frames || !low || low>high || high>frames || high>SIZE_MAX/2 || !std::isfinite(sigma) || sigma<=0)return false;
    std::size_t links=0,bytes=0;
    if(!prepared_storage::mul_bytes(frames/low-1,frames,links) || !prepared_storage::add_array(bytes,high,4*sizeof(float)) ||
       !prepared_storage::add_array(bytes,frames,4*sizeof(float)) || !prepared_storage::add_array(bytes,links,sizeof(std::size_t)) ||
       !prepared_storage::add_array(bytes,frames/low,sizeof(std::size_t)))return false;
    out=bytes;return true;
}
std::size_t PreparedIntervalSequenceDecoder::workspace_bytes() const noexcept {
    return (transition_.capacity()+log_transition_.capacity()+normalization_.capacity()+activation_.capacity()+previous_.capacity()+current_.capacity())*sizeof(float)+
        (backlinks_.capacity()+positions_.capacity())*sizeof(std::size_t);
}
}
