// madmom features/tempo.py ACF/Comb/DBNTempoHistogramProcessor recipes,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/tempo_histogram_sequence.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedTempoHistogramSequence::prepare(std::size_t frames,TempoHistogramSequenceConfig config,std::span<const float> alpha) {
    std::size_t bound=0;if(!storage_bytes(frames,config,alpha,bound))return false;
    PreparedTempoHistogramSequence next;
    if(!next.configure(frames,config,alpha))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedTempoHistogramSequence::configure(std::size_t frames,TempoHistogramSequenceConfig c,std::span<const float> alpha) {
    if(!frames || unsigned(c.method)>unsigned(TempoHistogramEstimator::StateModel))return false;
    if(!c.channels || (c.method!=TempoHistogramEstimator::Comb && (c.channels!=1 || !alpha.empty())))return false;
    IntervalHistogramConfig histogram;histogram.alpha=c.alpha;
    histogram.channels=c.channels;
    histogram.method=c.method==TempoHistogramEstimator::Autocorrelation?
        IntervalHistogramMethod::Autocorrelation:IntervalHistogramMethod::Comb;
    if(!interval_histogram_parameters(c.frame_rate,c.minimum_bpm,c.maximum_bpm,c.history_seconds,histogram) ||
        histogram.maximum_interval>UINT32_MAX)return false;
    config_=c;maximum_frames_=frames;
    coordinates_.resize(histogram.maximum_interval-histogram.minimum_interval+1);
    for(std::size_t i=0;i<coordinates_.size();++i)coordinates_[i]=float(histogram.minimum_interval+i);
    if(c.method==TempoHistogramEstimator::StateModel)return prepare_state_model(frames,histogram);
    return offline_.prepare(frames,histogram,alpha) && online_.prepare(frames,histogram,alpha);
}
bool PreparedTempoHistogramSequence::prepare_state_model(std::size_t frames,const IntervalHistogramConfig& h) {
    auto state=config_.state_model;state.frame_rate=config_.frame_rate;
    state.min_bpm=config_.minimum_bpm;state.max_bpm=config_.maximum_bpm;state.activation_threshold=0;
    if(!decoder_.prepare(frames,state))return false;
    const auto grid=decoder_.intervals();
    if(grid.empty() || grid.front()<h.minimum_interval || grid.back()>h.maximum_interval)return false;
    state_coordinates_.assign(grid.begin(),grid.end());
    state_bin_count_=std::size_t(grid.back()-grid.front())+1;
    if(!state_offline_.prepare(grid.front(),grid.back(),1) ||
        !state_online_.prepare(std::uint32_t(h.minimum_interval),std::uint32_t(h.maximum_interval),h.history_frames))return false;
    state_bins_.resize(std::max(coordinates_.size(),state_bin_count_));return true;
}
void PreparedTempoHistogramSequence::reset() noexcept {
    offline_.reset();online_.reset();decoder_.reset();state_offline_.reset();state_online_.reset();
    ready_=false;result_count_=0;
}
std::span<const float> PreparedTempoHistogramSequence::offline_coordinates() const noexcept {
    return config_.method==TempoHistogramEstimator::StateModel?std::span<const float>(state_coordinates_):std::span<const float>(coordinates_);
}
std::size_t PreparedTempoHistogramSequence::offline_bin_count() const noexcept {
    return config_.method==TempoHistogramEstimator::StateModel?state_bin_count_:coordinates_.size()*config_.channels;
}
std::span<const float> PreparedTempoHistogramSequence::channel_bins(std::size_t channel) const noexcept {
    if(!ready_ || channel>=config_.channels)return {};
    return config_.method==TempoHistogramEstimator::StateModel?bins():
        last_online_?online_.channel_bins(channel):offline_.channel_bins(channel);
}
bool PreparedTempoHistogramSequence::valid_input_size(std::size_t values) const noexcept {
    return maximum_frames_ && values%config_.channels==0 && values/config_.channels<=maximum_frames_;
}
std::span<const float> PreparedTempoHistogramSequence::bins() const noexcept {
    if(!ready_)return {};
    if(config_.method==TempoHistogramEstimator::StateModel)return std::span(state_bins_).first(result_count_);
    return last_online_?online_.bins():offline_.bins();
}
std::span<const float> PreparedTempoHistogramSequence::coordinates() const noexcept {
    if(!ready_)return {};
    return last_online_?online_coordinates():offline_coordinates();
}
bool PreparedTempoHistogramSequence::publish_counts(const PreparedStateIntervalHistogram& histogram) noexcept {
    const auto counts=histogram.counts();
    if(counts.size()>state_bins_.size())return false;
    for(std::size_t i=0;i<counts.size();++i){
        state_bins_[i]=float(counts[i]);
        if(!std::isfinite(state_bins_[i]))return false;
    }
    result_count_=counts.size();return true;
}
bool PreparedTempoHistogramSequence::process(std::span<const float> input,bool reset_state) noexcept {
    return config_.online?process_online(input,reset_state):process_offline(input);
}
bool PreparedTempoHistogramSequence::process_offline(std::span<const float> input) noexcept {
    ready_=false;last_online_=false;
    if(!valid_input_size(input.size()))return false;
    if(config_.method!=TempoHistogramEstimator::StateModel){ready_=offline_.process_offline(input);return ready_;}
    if(!decoder_.viterbi_states(input) || !state_offline_.offline(decoder_.state_path(),decoder_.state_intervals()))return false;
    ready_=publish_counts(state_offline_);return ready_;
}
bool PreparedTempoHistogramSequence::process_online(std::span<const float> input,bool reset_state) noexcept {
    ready_=false;last_online_=true;
    if(!valid_input_size(input.size()))return false;
    if(config_.method!=TempoHistogramEstimator::StateModel){ready_=online_.process_online(input,reset_state);return ready_;}
    if(!decoder_.forward_states(input,reset_state) ||
        !state_online_.online(decoder_.state_path(),decoder_.state_intervals(),reset_state))return false;
    ready_=publish_counts(state_online_);return ready_;
}
}


namespace dsp_kernels {
bool PreparedTempoHistogramSequence::storage_bytes(std::size_t frames,TempoHistogramSequenceConfig c,std::span<const float> alpha,std::size_t& out) noexcept {
    if(!frames || unsigned(c.method)>unsigned(TempoHistogramEstimator::StateModel) || !c.channels ||
       (c.method!=TempoHistogramEstimator::Comb && (c.channels!=1 || !alpha.empty())))return false;
    IntervalHistogramConfig h;h.alpha=c.alpha;h.channels=c.channels;
    h.method=c.method==TempoHistogramEstimator::Autocorrelation?IntervalHistogramMethod::Autocorrelation:IntervalHistogramMethod::Comb;
    if(!interval_histogram_parameters(c.frame_rate,c.minimum_bpm,c.maximum_bpm,c.history_seconds,h) || h.maximum_interval>UINT32_MAX)return false;
    const auto count=h.maximum_interval-h.minimum_interval+1;std::size_t bytes=0,nested=0;
    if(!prepared_storage::add_array(bytes,count,sizeof(float)))return false;
    if(c.method!=TempoHistogramEstimator::StateModel){
        if(!PreparedIntervalHistogram::storage_bytes(frames,h,alpha,nested) || !prepared_storage::add_bytes(bytes,nested) ||
           !prepared_storage::add_bytes(bytes,nested))return false;
    }else{
        auto state=c.state_model;state.frame_rate=c.frame_rate;state.min_bpm=c.minimum_bpm;state.max_bpm=c.maximum_bpm;state.activation_threshold=0;
        if(!PreparedBeatStateDecoder::storage_bytes(frames,state,nested) || !prepared_storage::add_bytes(bytes,nested) ||
           !PreparedStateIntervalHistogram::storage_bytes(std::uint32_t(h.minimum_interval),std::uint32_t(h.maximum_interval),1,nested) ||
           !prepared_storage::add_bytes(bytes,nested) ||
           !PreparedStateIntervalHistogram::storage_bytes(std::uint32_t(h.minimum_interval),std::uint32_t(h.maximum_interval),h.history_frames,nested) ||
           !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,count,2*sizeof(float)))return false;
    }
    out=bytes;return true;
}
std::size_t PreparedTempoHistogramSequence::workspace_bytes() const noexcept {
    return offline_.workspace_bytes()+online_.workspace_bytes()+decoder_.workspace_bytes()+state_offline_.workspace_bytes()+state_online_.workspace_bytes()+
        (coordinates_.capacity()+state_coordinates_.capacity()+state_bins_.capacity())*sizeof(float);
}
}
