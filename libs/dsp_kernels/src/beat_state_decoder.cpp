// Adapted from madmom features/beats_hmm.py and features/beats.py at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception,
// Johannes Kepler University, Linz, Austria and Austrian Research Institute
// for Artificial Intelligence (OFAI), Vienna, Austria. All rights reserved.
// BSD-style license and full disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/beat_state_decoder.h>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/metrical_state_space.h>
#include <dsp_kernels/metrical_observations.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace dsp_kernels {
namespace {
bool valid(const BeatStateDecoderConfig& c) noexcept {
    return std::isfinite(c.frame_rate) && c.frame_rate>0 && std::isfinite(c.frame_rate*60.f) &&
        std::isfinite(c.min_bpm) && c.min_bpm>0 && std::isfinite(c.max_bpm) && c.max_bpm>=c.min_bpm &&
        std::isfinite(c.transition_lambda) && std::isfinite(c.observation_lambda) && c.observation_lambda>1 &&
        std::isfinite(c.transition_floor) && c.transition_floor>=0 && c.transition_floor<1 &&
        std::isfinite(c.activation_threshold) && c.activation_threshold>=0 && c.activation_threshold<=1;
}
}
bool PreparedBeatStateDecoder::prepare(std::size_t frames,BeatStateDecoderConfig config) {
    std::size_t bound=0;if(!storage_bytes(frames,config,bound))return false;
    PreparedBeatStateDecoder next;if(!next.configure(frames,config))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatStateDecoder::configure(std::size_t frames,BeatStateDecoderConfig config) {
    if(!frames || !valid(config))return false;
    config_=config;maximum_frames_=frames;
    if(!build_intervals() || !build_states())return false;
    if(states_>UINT32_MAX || frames>SIZE_MAX/states_ || intervals_.size()>SIZE_MAX/intervals_.size())return false;
    links_.resize(frames*states_);path_.resize(frames);mask_.resize(frames);
    previous_.resize(states_);current_.resize(states_);forward_previous_.resize(states_);
    transition_.resize(intervals_.size()*intervals_.size());
    densities_.resize(states_);
    if(!build_transitions())return false;
    reset();return true;
}
bool PreparedBeatStateDecoder::build_intervals() {
    return discrete_period_grid(60.f*config_.frame_rate/config_.max_bpm,
                                60.f*config_.frame_rate/config_.min_bpm,config_.num_intervals,intervals_);
}
bool PreparedBeatStateDecoder::build_states() {
    std::size_t count=0,states=0;
    if(!period_grid_bound(60.f*config_.frame_rate/config_.max_bpm,60.f*config_.frame_rate/config_.min_bpm,count,states))return false;
    first_.reserve(count);last_.reserve(count);positions_.reserve(states);beat_state_.reserve(states);state_intervals_.reserve(states);
    for(const auto period:intervals_){
        if(states_>UINT32_MAX-period)return false;
        first_.push_back(std::uint32_t(states_));states_+=period;last_.push_back(std::uint32_t(states_-1));
        for(std::uint32_t phase=0;phase<period;++phase){
            const float position=float(phase)/float(period);positions_.push_back(position);
            beat_state_.push_back(std::uint8_t(position<1.f/config_.observation_lambda));state_intervals_.push_back(period);
        }
    }
    return states_>0;
}
bool PreparedBeatStateDecoder::build_transitions() {
    if(!exponential_period_transitions(intervals_,intervals_,
            {config_.transition_lambda,config_.fixed_interval},transition_,config_.transition_floor))return false;
    std::vector<MarkovTransition> edges;
    const auto count=intervals_.size();
    std::size_t capacity=states_;
    if(!prepared_storage::add_array(capacity,count,count))return false;
    edges.reserve(capacity);
    for(std::size_t interval=0;interval<count;++interval)
        for(auto state=first_[interval]+1;state<=last_[interval];++state)edges.push_back({state-1,state,1});
    for(std::size_t from=0;from<count;++from)
        for(std::size_t to=0;to<count;++to)
            if(transition_[from*count+to]>0)edges.push_back({last_[from],first_[to],transition_[from*count+to]});
    return transitions_.prepare(states_,edges);
}
void PreparedBeatStateDecoder::initialize(bool logarithmic) noexcept {
    std::fill(previous_.begin(),previous_.end(),logarithmic?std::log(1.f/float(states_)):1.f/float(states_));
}
void PreparedBeatStateDecoder::reset_online() noexcept {
    if(states_)std::fill(forward_previous_.begin(),forward_previous_.end(),1.f/float(states_));
    counter_=0;last_beat_frame_=0;online_tempo_=0;last_beat_seconds_=0;
}
void PreparedBeatStateDecoder::reset() noexcept {reset_online();result_frames_=0;path_start_=0;path_status_=MarkovResultStatus::Unprocessed;}
bool PreparedBeatStateDecoder::valid_activations(std::span<const float> values) const noexcept {
    return states_ && values.size()<=maximum_frames_ &&
        std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value) && value>=0 && value<=1;});
}
bool PreparedBeatStateDecoder::valid_input(std::span<const float> values,std::span<std::uint8_t> output) const noexcept {
    return output.size()==values.size() && valid_activations(values);
}
bool PreparedBeatStateDecoder::viterbi_states(std::span<const float> input) noexcept {
    result_frames_=0;path_start_=0;path_status_=MarkovResultStatus::InvalidInput;
    if(!valid_activations(input))return false;
    if(input.empty()){path_status_=MarkovResultStatus::Ready;return true;}
    return viterbi(input);
}
bool PreparedBeatStateDecoder::forward_states(std::span<const float> input,bool reset_state) noexcept {
    result_frames_=0;path_start_=0;path_status_=MarkovResultStatus::InvalidInput;
    if(!valid_activations(input))return false;
    if(reset_state)reset_online();
    path_status_=MarkovResultStatus::ArithmeticFailure;
    for(std::size_t i=0;i<input.size();++i){
        const auto state=forward(input[i]);if(state==states_)return false;
        path_[i]=std::uint32_t(state);
    }
    result_frames_=input.size();path_status_=MarkovResultStatus::Ready;return true;
}
float PreparedBeatStateDecoder::observation(float value,std::size_t state,bool logarithmic) const noexcept {
    return beat_activation_density(value,config_.observation_lambda,state!=0,logarithmic);
}
void PreparedBeatStateDecoder::load_observations(float value,bool logarithmic) noexcept {
    const std::array<float,2> density{observation(value,0,logarithmic),observation(value,1,logarithmic)};
    for(std::size_t state=0;state<states_;++state)densities_[state]=density[beat_state_[state]];
}
bool PreparedBeatStateDecoder::viterbi_frame(float value,std::size_t frame) noexcept {
    load_observations(value,true);
    if(!sparse_viterbi_step(transitions_.view(),densities_,previous_,current_,
                            std::span(links_).subspan(frame*states_,states_)))return false;
    previous_.swap(current_);return true;
}
bool PreparedBeatStateDecoder::viterbi(std::span<const float> input) noexcept {
    path_status_=MarkovResultStatus::ArithmeticFailure;initialize(true);
    for(std::size_t frame=0;frame<input.size();++frame)if(!viterbi_frame(input[frame],frame))return false;
    const auto best=std::max_element(previous_.begin(),previous_.end());
    if(*best==-std::numeric_limits<float>::infinity()){path_status_=MarkovResultStatus::NoPath;return true;}
    if(!std::isfinite(*best))return false;
    auto state=std::uint32_t(best-previous_.begin());
    for(std::size_t frame=input.size();frame-->0;){path_[frame]=state;state=links_[frame*states_+state];}
    result_frames_=input.size();path_status_=MarkovResultStatus::Ready;return true;
}
void PreparedBeatStateDecoder::correct_beats(std::span<const float> input,std::span<std::uint8_t> mask) const noexcept {
    std::size_t left=0;
    while(left<input.size()){
        if(!beat_state_[path_[left]]){++left;continue;}
        auto right=left+1;while(right<input.size() && beat_state_[path_[right]])++right;
        const auto peak=std::max_element(input.begin()+std::ptrdiff_t(left),input.begin()+std::ptrdiff_t(right));
        mask[std::size_t(peak-input.begin())]=1;left=right;
    }
}
void PreparedBeatStateDecoder::state_minima(std::span<std::uint8_t> mask) const noexcept {
    if(mask.size()<2)return;
    for(std::size_t i=0;i<mask.size();++i){
        const auto previous=(i+mask.size()-1)%mask.size(),next=(i+1)%mask.size();
        const float position=positions_[path_[i]];
        if(beat_state_[path_[i]] && position<positions_[path_[previous]] && position<positions_[path_[next]])mask[i]=1;
    }
}
void PreparedBeatStateDecoder::crop(std::span<const float> input,std::size_t& start,std::size_t& end) const noexcept {
    if(config_.activation_threshold<=0)return;
    while(start<end && input[start]<config_.activation_threshold)++start;
    while(end>start && input[end-1]<config_.activation_threshold)--end;
    if(start==0 && end==1)end=0; // Original threshold_activations idx.any().
}
bool PreparedBeatStateDecoder::offline_beats(std::span<const float> values,std::span<std::uint8_t> mask,std::size_t start) noexcept {
    if(values.empty() || !std::any_of(values.begin(),values.end(),[](float value){return value>0;}))return true;
    if(!viterbi(values))return false;
    if(path_status_==MarkovResultStatus::NoPath)return true;
    path_start_=start;
    // The source processor also rejects an all-zero Viterbi path (path.any()).
    if(std::none_of(path_.begin(),path_.begin()+std::ptrdiff_t(values.size()),[](auto state){return state!=0;}))return true;
    if(config_.correct)correct_beats(values,mask);else state_minima(mask);
    return true;
}
bool PreparedBeatStateDecoder::offline(std::span<const float> input,std::span<std::uint8_t> output) noexcept {
    path_status_=MarkovResultStatus::InvalidInput;
    if(!valid_input(input,output))return false;
    std::fill_n(mask_.begin(),input.size(),std::uint8_t{0});result_frames_=0;path_start_=0;path_status_=MarkovResultStatus::Ready;
    std::size_t start=0,end=input.size();crop(input,start,end);
    if(!offline_beats(input.subspan(start,end-start),std::span(mask_).subspan(start,end-start),start))return false;
    std::copy_n(mask_.begin(),input.size(),output.begin());return true;
}
std::size_t PreparedBeatStateDecoder::forward(float value) noexcept {
    load_observations(value,false);
    if(!sparse_forward_step(transitions_.view(),densities_,forward_previous_,current_))return states_;
    forward_previous_.swap(current_);
    return std::size_t(std::max_element(forward_previous_.begin(),forward_previous_.end())-forward_previous_.begin());
}
bool PreparedBeatStateDecoder::accept_online(std::size_t state,std::uint64_t frame) noexcept {
    if(!beat_state_[state] || frame<last_beat_frame_)return false;
    const float current=float(frame)/config_.frame_rate,next=last_beat_seconds_+60.f/config_.max_bpm;
    if(!std::isfinite(current) || current<next || current<=last_beat_seconds_)return false;
    online_tempo_=60.f/(current-last_beat_seconds_);
    last_beat_seconds_=current;last_beat_frame_=frame;return true;
}
bool PreparedBeatStateDecoder::online(std::span<const float> input,std::span<std::uint8_t> output,bool reset_state) noexcept {
    path_status_=MarkovResultStatus::InvalidInput;
    if(!valid_input(input,output))return false;
    if(reset_state)reset();
    if(input.size()>UINT64_MAX-counter_)return false;
    result_frames_=0;path_start_=0;path_status_=MarkovResultStatus::ArithmeticFailure;
    std::fill_n(mask_.begin(),input.size(),std::uint8_t{0});
    for(std::size_t i=0;i<input.size();++i){
        const auto state=forward(input[i]);if(state==states_)return false;path_[i]=std::uint32_t(state);
        mask_[i]=std::uint8_t(accept_online(state,counter_+i));
    }
    counter_+=input.size();result_frames_=input.size();path_status_=MarkovResultStatus::Ready;
    std::copy_n(mask_.begin(),input.size(),output.begin());return true;
}
}

namespace dsp_kernels {
bool PreparedBeatStateDecoder::storage_bytes(std::size_t frames,BeatStateDecoderConfig c,std::size_t& out) noexcept {
    std::size_t count=0,states=0,edges=0,square=0,links=0,bytes=0;
    if(!frames || !valid(c) || !period_grid_bound(60.f*c.frame_rate/c.max_bpm,60.f*c.frame_rate/c.min_bpm,count,states) ||
       states>UINT32_MAX || !prepared_storage::mul_bytes(count,count,square) || !prepared_storage::add_bytes(edges,states) ||
       !prepared_storage::add_bytes(edges,square) || !PreparedSparseMarkovTransitions::storage_bytes(states,edges,bytes) ||
       !prepared_storage::mul_bytes(frames,states,links))return false;
    if(!prepared_storage::add_array(bytes,count,3*sizeof(std::uint32_t)) ||
       !prepared_storage::add_array(bytes,states,sizeof(std::uint32_t)+sizeof(std::uint8_t)+5*sizeof(float)) ||
       !prepared_storage::add_array(bytes,links,sizeof(std::uint32_t)) || !prepared_storage::add_array(bytes,frames,sizeof(std::uint32_t)+sizeof(std::uint8_t)) ||
       !prepared_storage::add_array(bytes,square,sizeof(float)) || !prepared_storage::add_array(bytes,edges,sizeof(MarkovTransition)))return false;
    out=bytes;return true;
}
std::size_t PreparedBeatStateDecoder::workspace_bytes() const noexcept {
    return (intervals_.capacity()+first_.capacity()+last_.capacity()+links_.capacity()+path_.capacity()+state_intervals_.capacity())*sizeof(std::uint32_t)+
        beat_state_.capacity()+mask_.capacity()+transitions_.workspace_bytes()+
        (positions_.capacity()+transition_.capacity()+densities_.capacity()+previous_.capacity()+current_.capacity()+forward_previous_.capacity())*sizeof(float);
}
}
