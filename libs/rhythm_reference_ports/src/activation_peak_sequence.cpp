// OnsetPeakPickingProcessor and BufferProcessor semantics adapted from
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
// Recipe uses the existing GPL scalar storage; no GPL dependency enters D.
#include <rhythm_reference_ports/activation_peak_sequence.h>
#include <dsp_kernels/frame_timing.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
bool timing_parameters(const ActivationPeakSequenceConfig& c,dsp_kernels::ActivationPeakConfig& p,
                       std::size_t& smoothing) noexcept {
    p.threshold=c.threshold;
    return dsp_kernels::rounded_frame_count(c.smooth,c.frame_rate,smoothing) &&
        dsp_kernels::rounded_frame_count(c.pre_average,c.frame_rate,p.pre_average) &&
        dsp_kernels::rounded_frame_count(c.post_average,c.frame_rate,p.post_average) &&
        dsp_kernels::rounded_frame_count(c.pre_maximum,c.frame_rate,p.pre_maximum) &&
        dsp_kernels::rounded_frame_count(c.post_maximum,c.frame_rate,p.post_maximum);
}
bool valid_config(const ActivationPeakSequenceConfig& c) noexcept {
    return std::isfinite(c.threshold) && std::isfinite(c.combine) && c.combine>=0 &&
        std::isfinite(c.delay) && (!c.online || c.delay==0);
}
}
bool PreparedActivationPeakSequence::prepare(std::size_t maximum,ActivationPeakSequenceConfig config) {
    PreparedActivationPeakSequence next;std::size_t bytes=0;
    if(!storage_bytes(maximum,config,bytes) || !next.configure(maximum,config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
namespace {
void causal_peak_config(ActivationPeakSequenceConfig& config) noexcept {
    if(config.online){config.smooth=0;config.post_average=0;config.post_maximum=0;}
}
bool peak_sequence_storage(std::size_t buffered,std::size_t count,std::size_t smoothing,std::size_t& bytes) noexcept {
    std::size_t history=0;
    using namespace dsp_kernels::prepared_storage;
    return ScalarHistory::storage_bytes(buffered,history) && add_bytes(bytes,history) &&
        add_array(bytes,smoothing,2*sizeof(float)) && add_array(bytes,count,4*sizeof(float)+3*sizeof(std::uint8_t)) &&
        add_array(bytes,count+1,2*sizeof(float));
}
}
bool PreparedActivationPeakSequence::storage_bytes(std::size_t maximum,ActivationPeakSequenceConfig config,std::size_t& output) noexcept {
    if(!maximum || maximum>std::size_t(INT32_MAX/4) || !valid_config(config))return false;
    causal_peak_config(config);
    dsp_kernels::ActivationPeakConfig peak;std::size_t smoothing=0;
    if(!timing_parameters(config,peak,smoothing) || smoothing==1 || peak.pre_maximum>SIZE_MAX-maximum)return false;
    const auto buffered=maximum+peak.pre_maximum,count=std::max(buffered,smoothing);
    if(count==SIZE_MAX)return false;
    // Each D kernel owns <=kernel +2*max(input,kernel) floats and flags.
    std::size_t bytes=0;
    if(!peak_sequence_storage(buffered,count,smoothing,bytes))return false;
    output=bytes;return true;
}
std::size_t PreparedActivationPeakSequence::workspace_bytes() const noexcept {
    return offline_.workspace_bytes()+online_.workspace_bytes()+history_.workspace_bytes()+peaks_.capacity()+
        (raw_.capacity()+events_.capacity()+smoothing_.capacity())*sizeof(float);
}
bool PreparedActivationPeakSequence::configure(std::size_t maximum,ActivationPeakSequenceConfig c) {
    if(!maximum || maximum>std::size_t(INT32_MAX/4) || !valid_config(c))return false;
    causal_peak_config(c);
    config_=c;maximum_frames_=maximum;std::size_t smoothing=0;
    if(!timing_parameters(c,peak_config_,smoothing) || smoothing==1)return false;
    smoothing_.resize(smoothing);
    if(!dsp_kernels::symmetric_hamming_window(smoothing_))return false;
    return prepare_kernels();
}
bool PreparedActivationPeakSequence::prepare_kernels() {
    const auto buffered=maximum_frames_+peak_config_.pre_maximum;
    const auto output=std::max(buffered,smoothing_.size());
    if(!offline_.prepare(maximum_frames_,peak_config_,smoothing_))return false;
    auto causal=peak_config_;causal.post_average=0;causal.post_maximum=0;
    if(!online_.prepare(buffered,causal) || !history_.prepare(buffered))return false;
    peaks_.resize(output);raw_.resize(output+1);events_.resize(output+1);
    reset();return true;
}
void PreparedActivationPeakSequence::reset() noexcept {
    started_=false;has_last_=false;counter_=0;event_count_=0;raw_count_=0;last_onset_=0;
}
bool PreparedActivationPeakSequence::validate(std::span<const float> input) const noexcept {
    return maximum_frames_ && input.size()<=maximum_frames_ &&
        std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);});
}
bool PreparedActivationPeakSequence::process(std::span<const float> input,bool reset_state) noexcept {
    return config_.online?process_online(input,reset_state):process_offline(input);
}
bool PreparedActivationPeakSequence::collect(dsp_kernels::PreparedActivationPeaks& kernel,std::span<const float> input,std::int64_t offset,float delay) noexcept {
    const auto count=kernel.output_frames(input.size());
    if(!kernel.process(input,std::span(peaks_).first(count)))return false;
    raw_count_=0;
    for(std::size_t i=0;i<count;++i){
        if(!peaks_[i])continue;
        const float seconds=float(offset+std::int64_t(i))/config_.frame_rate+delay;
        if(!std::isfinite(seconds))return false;
        raw_[raw_count_++]=seconds;
    }
    return true;
}
void PreparedActivationPeakSequence::prepend_previous() noexcept {
    std::move_backward(raw_.begin(),raw_.begin()+std::ptrdiff_t(raw_count_),raw_.begin()+std::ptrdiff_t(raw_count_+1));
    raw_[0]=last_onset_;
}
void PreparedActivationPeakSequence::publish_combined(std::size_t count,std::size_t skip,bool remember) noexcept {
    if(remember && count){last_onset_=events_[count-1];has_last_=true;}
    event_count_=count-skip;
    if(skip)std::move(events_.begin()+1,events_.begin()+std::ptrdiff_t(count),events_.begin());
}
bool PreparedActivationPeakSequence::combine(bool previous,bool remember) noexcept {
    const auto skip=std::size_t(previous);
    if(previous)prepend_previous();
    std::size_t count=0;
    if(!dsp_kernels::combine_event_times(std::span(raw_).first(raw_count_+skip),config_.combine,
        dsp_kernels::EventCombination::Left,events_,count,dsp_kernels::EventOrdering::Sequence))return false;
    if(previous && events_[count-1]==last_onset_){event_count_=0;return true;}
    publish_combined(count,skip,remember);
    return true;
}
bool PreparedActivationPeakSequence::process_offline(std::span<const float> input) noexcept {
    event_count_=0;
    if(!validate(input))return false;
    if(input.empty())return true;
    if(!collect(offline_,input,0,config_.delay))return false;
    if(config_.combine)return combine(false);
    std::copy_n(raw_.begin(),raw_count_,events_.begin());event_count_=raw_count_;return true;
}
bool PreparedActivationPeakSequence::update_buffer(std::span<const float> input,bool reset_state) noexcept {
    if(reset_state || !started_){
        reset();
        if(!history_.reset_length(peak_config_.pre_maximum+input.size()))return false;
        counter_=-std::int64_t(peak_config_.pre_maximum);started_=true;
    }
    if(counter_>std::numeric_limits<std::int64_t>::max()-std::int64_t(input.size())-std::int64_t(history_.values().size()))return false;
    for(float value:input)history_.push(value);
    return true;
}
bool PreparedActivationPeakSequence::combine_online() noexcept {
    const bool any_nonzero=std::any_of(raw_.begin(),raw_.begin()+std::ptrdiff_t(raw_count_),[](float x){return x!=0;});
    if(config_.combine && any_nonzero)return combine(has_last_,true);
    std::copy_n(raw_.begin(),raw_count_,events_.begin());event_count_=raw_count_;return true;
}
bool PreparedActivationPeakSequence::process_online(std::span<const float> input,bool reset_state) noexcept {
    event_count_=0;
    if(!config_.online || !validate(input))return false;
    if(input.empty()){if(reset_state)reset();return true;}
    if(!update_buffer(input,reset_state))return false;
    if(!collect(online_,history_.values(),counter_,0))return false;
    counter_+=std::int64_t(input.size());
    return combine_online();
}
}
