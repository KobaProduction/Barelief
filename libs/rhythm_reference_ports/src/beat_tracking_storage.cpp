// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/global_onset.h>
#include <rhythm_reference_ports/period_path.h>
#include <rhythm_reference_ports/renewal_beat_decoder.h>
#include <rhythm_reference_ports/beat_agreement.h>
#include <rhythm_reference_ports/lag_tempo_vote.h>
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
namespace {
using namespace dsp_kernels::prepared_storage;
template<class T> std::size_t payload(const std::vector<T>& v) noexcept {return v.capacity()*sizeof(T);}
template<class... T> std::size_t payloads(const T&... v) noexcept {return (payload(v)+...+std::size_t{});}
bool positive(float v) noexcept {return std::isfinite(v) && v>0;}
bool extent(float x,std::size_t& out) noexcept {
    if(!std::isfinite(x) || x<0 || double(x)>=double(PTRDIFF_MAX))return false;
    out=std::size_t(x);return true;
}
bool add_float(std::size_t& b,std::size_t n,std::size_t multiplier=1) noexcept {std::size_t k=0;return mul_bytes(n,multiplier,k) && add_array(b,k,sizeof(float));}
bool period_geometry(PeriodPathConfig& c) noexcept {
    if(!positive(c.frame_rate) || c.frame_rate<1 || !positive(c.frame_duration_s) || !positive(c.min_bpm) || !positive(c.max_bpm) || c.max_bpm<c.min_bpm+20 || !std::isfinite(c.noise_amount) || c.noise_amount<0)return false;
    std::size_t n=0;if(!extent(std::round(c.frame_duration_s*c.frame_rate),n))return false;
    if(!c.frame_length)c.frame_length=n;if(!c.hop)c.hop=c.frame_length/4;
    return c.frame_length>=16 && c.frame_length<UINT32_MAX && c.hop>=2 && c.hop<UINT32_MAX && (c.frame_length-3)/4-1<c.hop;
}
}
bool PreparedInformationGain::storage_bytes(std::size_t bins,std::size_t first,std::size_t stop,std::size_t& b) noexcept {
    b=0;return bins && first<stop && stop<=bins && add_float(b,stop-first,11);
}
std::size_t PreparedInformationGain::workspace_bytes() const noexcept {return payload(history_);}
bool PreparedErbFilterBank::storage_bytes(std::size_t bins,ErbFilterBankConfig c,std::size_t& b) noexcept {
    b=0;std::size_t n=0;
    return bins>=2 && c.bands && positive(c.sample_rate) && std::isfinite(c.low_hz) && c.low_hz>=0 && positive(c.high_hz) && c.high_hz>c.low_hz && c.high_hz<=c.sample_rate*.5f && positive(c.width) &&
        mul_bytes(c.bands,bins,n) && add_float(b,n) && add_float(b,c.bands,2);
}
std::size_t PreparedErbFilterBank::workspace_bytes() const noexcept {return payloads(weights_,centers_,staging_);}
bool PreparedPeriodPath::storage_bytes(std::size_t n,PeriodPathConfig c,std::size_t& b) noexcept {
    b=0;if(!n || !period_geometry(c))return false;
    std::size_t square=0,observations=0;
    return mul_bytes(c.hop,c.hop,square) && mul_bytes(n,c.hop,observations) && add_float(b,c.hop,4) &&
        add_float(b,square) && add_float(b,observations) && add_array(b,observations,sizeof(std::uint32_t)) && add_array(b,n,sizeof(std::uint32_t));
}
std::size_t PreparedPeriodPath::workspace_bytes() const noexcept {
    return payloads(filtered_,weights_,transition_,observations_,previous_,current_,predecessor_,path_);
}
bool PreparedRenewalBeatDecoder::storage_bytes(std::size_t samples,std::size_t blocks,RenewalBeatConfig c,std::size_t& b) noexcept {
    b=0;if(!samples || !blocks || c.upsampling<1 || c.upsampling>4 || !positive(c.frame_rate) || !positive(c.maximum_period_s) || !positive(c.sigma_ibi) || !positive(c.transition_mix) || c.transition_mix>=1)return false;
    PreparedRenewalBeatDecoder v;v.config_=c;v.rate_=c.frame_rate*float(c.upsampling);v.resolution_=1.f/v.rate_;
    std::size_t states=0,radius=0,models=0;
    if(!positive(v.rate_) || !v.state_count(c.maximum_period_s,states) || !extent(std::ceil(4.f*c.sigma_ibi/v.resolution_),radius) || radius>(SIZE_MAX-1)/2 || !mul_bytes(blocks,states,models))return false;
    return add_float(b,2*radius+1) && add_float(b,models,2) && add_float(b,states,2) && add_float(b,samples) && add_array(b,samples,2*sizeof(std::uint32_t)+sizeof(std::uint8_t));
}
std::size_t PreparedRenewalBeatDecoder::workspace_bytes() const noexcept {return payloads(gaussian_,reset_log_,advance_log_,previous_,current_,onset_,reset_from_,path_,beats_);}
bool PreparedBeatAgreement::storage_bytes(std::size_t methods,std::size_t beats,float minimum,std::size_t& b) noexcept {
    b=0;std::size_t pairs=0;return methods && beats && std::isfinite(minimum) && minimum>=0 && mul_bytes(methods,methods,pairs) && add_float(b,methods) && add_float(b,pairs) && add_float(b,128);
}
std::size_t PreparedBeatAgreement::workspace_bytes() const noexcept {return payloads(boundaries_,histogram_,scores_,pairwise_);}
bool PreparedLagTempoVote::storage_bytes(std::size_t n,LagTempoVoteConfig c,std::size_t& b) noexcept {
    b=0;std::size_t bins=0;
    return n && c.audio_hop && positive(c.sample_rate) && positive(c.maximum_bpm) && c.maximum_bpm<=std::nextafter(float(INT_MAX),0.f) && extent(c.maximum_bpm/2.f+.5f,bins) && add_float(b,n,2) && add_array(b,bins+1,sizeof(std::uint64_t));
}
std::size_t PreparedLagTempoVote::workspace_bytes() const noexcept {return payloads(bpms_,estimates_,counts_);}
} // namespace rhythm_reference_ports
