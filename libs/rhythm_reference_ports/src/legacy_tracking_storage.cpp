// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/period_phase_ticks.h>
#include <rhythm_reference_ports/band_tempo_features.h>
#include <rhythm_reference_ports/frame_silence_policy.h>
#include <rhythm_reference_ports/lag_observation_pool.h>
#include <rhythm_reference_ports/tick_sequence_refinement.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
namespace {
using namespace dsp_kernels::prepared_storage;
template<class T> std::size_t bytes(const std::vector<T>& v) noexcept {return v.capacity()*sizeof(T);}
template<class... T> std::size_t owned(const T&... v) noexcept {return (bytes(v)+...+std::size_t{});}
bool positive(float v) noexcept {return std::isfinite(v) && v>0;}
bool floats(std::size_t& b,std::size_t n,std::size_t m=1) noexcept {std::size_t count=0;return mul_bytes(n,m,count) && add_array(b,count,sizeof(float));}
}
bool PreparedBandTempoFeatures::storage_bytes(std::span<const float> gains,float time,std::size_t& b) noexcept {
    b=0;return !gains.empty() && positive(time) && std::isfinite(std::sqrt(256.f/time)) && std::all_of(gains.begin(),gains.end(),[](float v){return std::isfinite(v);}) && floats(b,gains.size(),4);
}
std::size_t PreparedBandTempoFeatures::workspace_bytes() const noexcept {return owned(gains_,previous_,current_,result_);}
bool PreparedPeriodPhaseTicks::storage_bytes(std::size_t n,std::size_t matches,std::size_t ticks,PeriodPhaseTickConfig c,std::size_t& b) noexcept {
    b=0;if(!n || n>std::size_t(PTRDIFF_MAX)-12 || !matches || !ticks || !c.evaluation_hop || c.evaluation_hop>INT_MAX || !c.audio_hop || !positive(c.sample_rate) || !positive(c.maximum_candidate_frames) || c.maximum_candidate_frames>std::nextafter(float(INT_MAX),0.f))return false;
    const auto count=std::size_t(c.maximum_candidate_frames/2.f+.5f)+1;
    return positive(float(c.audio_hop)/c.sample_rate) && floats(b,n) && floats(b,n+12) && floats(b,matches) && floats(b,ticks) && add_array(b,count,sizeof(std::uint64_t));
}
std::size_t PreparedPeriodPhaseTicks::workspace_bytes() const noexcept {return owned(periods_,phases_,matches_,ticks_,counts_);}
bool PreparedFrameSilencePolicy::storage_bytes(std::size_t n,FrameSilenceConfig c,std::size_t& b) noexcept {
    b=0;return n && std::isfinite(c.threshold_power) && c.threshold_power>=0 && std::isfinite(c.noise_trigger_power) && c.noise_trigger_power>=0 && std::isfinite(c.noise_gain) && c.noise_gain>=0 && unsigned(c.boundaries)<=1 && floats(b,n,2);
}
std::size_t PreparedFrameSilencePolicy::workspace_bytes() const noexcept {return owned(ones_,scratch_);}
bool PreparedLagObservationPool::storage_bytes(std::size_t rows,std::size_t values,std::size_t flat,std::size_t& b) noexcept {
    b=0;return rows && values && flat && floats(b,values) && add_array(b,rows,sizeof(Row));
}
std::size_t PreparedLagObservationPool::workspace_bytes() const noexcept {return owned(values_,rows_);}
bool PreparedTickSequenceRefinement::storage_bytes(std::size_t n,TickSequenceRefinementConfig c,std::size_t& b) noexcept {
    b=0;return n && positive(c.frame_rate) && std::isfinite(c.minimum_interval_seconds) && c.minimum_interval_seconds>=0 && std::isfinite(c.last_beat_interval_seconds) && c.last_beat_interval_seconds>=0 && floats(b,n,2);
}
std::size_t PreparedTickSequenceRefinement::workspace_bytes() const noexcept {return owned(ticks_,intervals_);}
} // namespace rhythm_reference_ports
