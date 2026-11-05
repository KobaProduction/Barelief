// madmom features/beats.py CRFBeatDetectionProcessor composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/crf_beat_sequence.h>
#include <dsp_kernels/convolution.h>
#include <dsp_kernels/frame_timing.h>
#include <dsp_kernels/window_math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <limits>
namespace dsp_kernels {
namespace {
bool crf_input_valid(std::size_t frames,CrfBeatSequenceConfig c,std::span<const float> factors) noexcept {
    if(!frames || !c.num_intervals || (c.use_factors && factors.empty()) ||
       !std::all_of(factors.begin(),factors.end(),[](float x){return std::isfinite(x);}))return false;
    return true;
}
bool crf_settings_valid(std::size_t frames,CrfBeatSequenceConfig c,std::span<const float> factors) noexcept {
    if(!crf_input_valid(frames,c,factors))return false;
    return !(c.tempo.precision==NumericalPrecision::DesktopReference && c.use_factors);
}
bool crf_frame_bounds(CrfBeatSequenceConfig c,IntervalHistogramConfig& limits,std::size_t& smoothing) noexcept {
    const auto& h=c.tempo.histogram;
    if(!interval_histogram_parameters(h.frame_rate,h.minimum_bpm,h.maximum_bpm,h.history_seconds,limits) ||
       !truncated_frame_count(c.tempo.activation_smoothing_seconds,h.frame_rate,smoothing))return false;
    return true;
}
bool crf_storage_arrays(std::size_t& bytes,std::size_t frames,std::size_t smoothing,std::size_t effective,std::size_t trials,std::size_t factors,std::size_t minimum,NumericalPrecision precision) noexcept {
    const auto width=precision==NumericalPrecision::DesktopReference?sizeof(double):sizeof(float);
    if(!prepared_storage::add_array(bytes,factors,sizeof(float)) || !prepared_storage::add_array(bytes,smoothing,width) ||
       !prepared_storage::add_array(bytes,trials,sizeof(std::size_t)) || !prepared_storage::add_array(bytes,effective,sizeof(float)) ||
       !prepared_storage::add_array(bytes,effective/minimum,sizeof(float)))return false;
    return precision!=NumericalPrecision::DesktopReference ||
        (prepared_storage::add_array(bytes,frames,sizeof(double)) && prepared_storage::add_array(bytes,effective,sizeof(double)));
}
template<typename Scalar,typename Candidate> bool top_beat_intervals(std::span<const Candidate> candidates,Scalar numerator,std::size_t limit,std::span<std::size_t> output,std::size_t& trials) noexcept {
    const auto count=std::min(candidates.size(),limit);
    for(std::size_t i=0;i<count;++i){
        const Scalar interval=numerator/candidates[i].tempo;
        if(!std::isfinite(interval) || interval<1 || interval>=Scalar(SIZE_MAX))return false;
        output[trials++]=std::size_t(interval);
    }
    return trials>0;
}
}
bool PreparedCrfBeatSequence::prepare(std::size_t maximum,CrfBeatSequenceConfig config,std::span<const float> factors) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,factors,bound))return false;
    PreparedCrfBeatSequence next;
    if(!next.configure(maximum,config,factors))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedCrfBeatSequence::configure(std::size_t frames,CrfBeatSequenceConfig c,std::span<const float> factors) {
    if(!crf_input_valid(frames,c,factors))return false;
    IntervalHistogramConfig bounds;std::size_t smoothing=0;
    if(!crf_frame_bounds(c,bounds,smoothing))return false;
    config_=c;maximum_frames_=frames;minimum_=bounds.minimum_interval;maximum_=bounds.maximum_interval;
    factors_.assign(factors.begin(),factors.end());
    if(!prepare_window(frames,smoothing))return false;
    return prepare_numerics(frames,std::max(frames,smoothing));
}
bool PreparedCrfBeatSequence::prepare_window(std::size_t maximum,std::size_t smoothing) {
    if(config_.tempo.precision==NumericalPrecision::DesktopReference){
        reference_kernel_.resize(smoothing);reference_input_.resize(maximum);reference_smoothed_.resize(std::max(maximum,smoothing));
        if(!symmetric_hamming_window(reference_kernel_))return false;
    }else{activation_kernel_.resize(smoothing);if(!symmetric_hamming_window(activation_kernel_))return false;}
    return true;
}
bool PreparedCrfBeatSequence::prepare_numerics(std::size_t raw,std::size_t effective) {
    const auto trials=config_.use_factors?factors_.size():config_.num_intervals;
    if(!tempo_.prepare(raw,config_.tempo) || !search_.prepare(std::max(effective,maximum_),minimum_,maximum_,trials,config_.interval_sigma))return false;
    intervals_.resize(trials);activations_.resize(effective);events_.resize(effective/minimum_);return true;
}
bool PreparedCrfBeatSequence::select_intervals() noexcept {
    if(config_.tempo.precision==NumericalPrecision::DesktopReference)return select_reference_intervals();
    trials_=0;const auto candidates=tempo_.candidates();const float numerator=60.f*config_.tempo.histogram.frame_rate;
    if(candidates.empty() || !std::isfinite(candidates[0].tempo) || candidates[0].tempo<=0)return false;
    if(config_.use_factors)return factor_beat_intervals(numerator/candidates[0].tempo,factors_,minimum_,maximum_,intervals_,trials_);
    return top_beat_intervals<float,HistogramTempo>(candidates,numerator,config_.num_intervals,intervals_,trials_);
}
bool PreparedCrfBeatSequence::publish() noexcept {
    const auto positions=search_.positions();
    if(positions.size()>events_.size())return false;
    for(std::size_t i=0;i<positions.size();++i){
        const float time=float(positions[i])/config_.tempo.histogram.frame_rate;
        if(!std::isfinite(time))return false;
        events_[i]=time;
    }
    count_=positions.size();ready_=true;outcome_=SequenceDecodeOutcome::Path;error_reason_=nullptr;return true;
}
void PreparedCrfBeatSequence::clear_result() noexcept {
    ready_=false;tempo_ready_=false;search_attempted_=false;count_=0;trials_=0;
    outcome_=SequenceDecodeOutcome::Error;error_reason_="not_processed";
}
bool PreparedCrfBeatSequence::no_path() noexcept {
    ready_=false;count_=0;outcome_=SequenceDecodeOutcome::NoPath;error_reason_=nullptr;return true;
}
bool PreparedCrfBeatSequence::fail(const char* reason) noexcept {
    clear_result();error_reason_=reason;return false;
}
bool PreparedCrfBeatSequence::no_tempo() const noexcept {
    const auto candidates=tempo_.candidates();
    return candidates.empty() || (candidates.size()==1 && std::isnan(candidates[0].tempo) && candidates[0].strength==0);
}
bool PreparedCrfBeatSequence::valid_input(std::span<const float> input) const noexcept {
    return maximum_frames_ && input.size()<=maximum_frames_ &&
        std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x) && x>=0;});
}
bool PreparedCrfBeatSequence::decode(std::span<const float> input) noexcept {
    auto smoothed=std::span(activations_).first(same_convolution_size(input.size(),config_.tempo.precision==NumericalPrecision::DesktopReference?reference_kernel_.size():activation_kernel_.size()));
    if(!smooth(input,smoothed))return fail("smoothing_failed");
    search_attempted_=true;
    if(!search_.process(smoothed,std::span(intervals_).first(trials_))){
        if(search_.outcome()==SequenceDecodeOutcome::NoPath)return no_path();
        return fail(search_.error_reason());
    }
    return publish() || fail("publication_overflow");
}
bool PreparedCrfBeatSequence::process(std::span<const float> input) noexcept {
    clear_result();if(!valid_input(input))return fail("invalid_input");
    if(input.empty())return no_path();
    if(!tempo_.process(input))return fail("tempo_failed");
    tempo_ready_=true;if(no_tempo())return no_path();
    if(!select_intervals())return fail("invalid_intervals");
    if(!trials_)return no_path();
    return decode(input);
}
}


namespace dsp_kernels {
bool PreparedCrfBeatSequence::storage_bytes(std::size_t frames,CrfBeatSequenceConfig c,std::span<const float> factors,std::size_t& out) noexcept {
    if(!crf_settings_valid(frames,c,factors))return false;
    IntervalHistogramConfig limits;std::size_t smoothing=0;
    if(!crf_frame_bounds(c,limits,smoothing))return false;
    if(c.tempo.precision==NumericalPrecision::DesktopReference && smoothing==1)return false;
    const auto effective=std::max(frames,smoothing),trials=c.use_factors?factors.size():c.num_intervals;
    std::size_t bytes=0,nested=0;
    if(!PreparedTempoAnalysisSequence::storage_bytes(frames,c.tempo,{},{},{},bytes) ||
       !PreparedIntervalSequenceSearch::storage_bytes(std::max(effective,limits.maximum_interval),limits.minimum_interval,limits.maximum_interval,trials,c.interval_sigma,nested) ||
       !prepared_storage::add_bytes(bytes,nested) ||
       !crf_storage_arrays(bytes,frames,smoothing,effective,trials,factors.size(),limits.minimum_interval,c.tempo.precision))return false;
    out=bytes;return true;
}
std::size_t PreparedCrfBeatSequence::workspace_bytes() const noexcept {
    return (reference_kernel_.capacity()+reference_input_.capacity()+reference_smoothed_.capacity())*sizeof(double)+tempo_.workspace_bytes()+search_.workspace_bytes()+(factors_.capacity()+activation_kernel_.capacity()+activations_.capacity()+events_.capacity())*sizeof(float)+
        intervals_.capacity()*sizeof(std::size_t);
}
bool PreparedCrfBeatSequence::select_reference_intervals() noexcept {
    trials_=0;const auto candidates=tempo_.reference_candidates();
    const double numerator=60.*double(config_.tempo.histogram.frame_rate);
    return top_beat_intervals<double,HistogramTempoReference>(candidates,numerator,config_.num_intervals,intervals_,trials_);
}
bool PreparedCrfBeatSequence::smooth(std::span<const float> input,std::span<float> output) noexcept {
    if(config_.tempo.precision!=NumericalPrecision::DesktopReference)return convolve_same(input,activation_kernel_,output);
    std::copy(input.begin(),input.end(),reference_input_.begin());
    auto values=std::span(reference_smoothed_).first(output.size());
    if(!convolve_same(std::span<const double>(reference_input_).first(input.size()),reference_kernel_,values) ||
       !std::all_of(values.begin(),values.end(),[](double x){return std::isfinite(x) && std::abs(x)<=double(std::numeric_limits<float>::max());}))return false;
    std::transform(values.begin(),values.end(),output.begin(),[](double x){return float(x);});return true;
}

}
