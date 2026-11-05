// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/pcm_beat_recipe.h>
#include <rhythm_reference_ports/legacy_rhythm_recipe.h>
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
bool front(std::size_t samples,std::size_t length,std::size_t hop,bool centered,std::size_t& frames,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(!samples || samples>INT_MAX || length<2 || length>INT_MAX || !hop || hop>INT_MAX)return false;
    dsp_kernels::TemporalWindowConfig c;c.length=length;c.hop=hop;c.center=centered;c.periodic=false;
    c.tail=centered?dsp_kernels::TemporalTail::CenterCoverEnd:dsp_kernels::TemporalTail::CoverEnd;c.odd_center_ceil=centered;
    return dsp_kernels::PreparedTemporalWindow::frame_count(c,samples,frames) && frames &&
        dsp_kernels::PreparedFourierTempogram::storage_bytes(c,{},length,true,bytes,factory) && add_float(bytes,length);
}
bool period_geometry(PeriodPathConfig& c) noexcept {
    if(!positive(c.frame_rate) || c.frame_rate<1 || !positive(c.frame_duration_s) || !positive(c.min_bpm) || !positive(c.max_bpm) || c.max_bpm<c.min_bpm+20 || !std::isfinite(c.noise_amount) || c.noise_amount<0)return false;
    std::size_t n=0;if(!extent(std::round(c.frame_duration_s*c.frame_rate),n))return false;
    if(!c.frame_length)c.frame_length=n;if(!c.hop)c.hop=c.frame_length/4;
    return c.frame_length>=16 && c.frame_length<UINT32_MAX && c.hop>=2 && c.hop<UINT32_MAX && (c.frame_length-3)/4-1<c.hop;
}
bool tracker_geometry(std::size_t n,PeriodBeatTrackingConfig c,std::size_t& expanded,std::size_t& blocks,PeriodPathConfig& p,dsp_kernels::TemporalWindowConfig& w) noexcept {
    if(!n || !positive(c.frame_rate) || c.upsampling<1 || c.upsampling>4 || !dsp_kernels::endpoint_expansion_size(n,c.upsampling,expanded))return false;
    p.frame_rate=c.frame_rate*float(c.upsampling);p.min_bpm=c.min_bpm;p.max_bpm=c.max_bpm;p.noise=c.noise;
    if(!period_geometry(p))return false;
    w.length=p.frame_length;w.hop=p.hop;w.center=false;w.periodic=false;w.function=dsp_kernels::WindowFunction::Rectangular;w.tail=dsp_kernels::TemporalTail::CoverEnd;
    return dsp_kernels::PreparedTemporalWindow::frame_count(w,expanded,blocks) && blocks;
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
bool PreparedBandBeatEmphasis::storage_bytes(std::size_t n,BandBeatEmphasisConfig c,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    b=0;std::size_t p=0,rows=0,scratch=0;
    if(!n || n>SIZE_MAX-16 || !c.bands || !positive(c.frame_rate) || !extent(std::round(5.944308390022676f*c.frame_rate),p) || p<32 || std::round(43.f*512.f/float(p))<=0)return false;
    if(!mul_bytes(c.bands,n+16,rows) || !PreparedCausalAverage::storage_bytes(17,b) || !dsp_kernels::PreparedVariableAutocorrelation::storage_bytes(n+16,dsp_kernels::CorrelationNormalization::Unbiased,scratch,factory))return false;
    return add_bytes(b,scratch) && add_float(b,rows) && add_float(b,c.bands,3) && add_float(b,p,2) && add_float(b,n);
}
std::size_t PreparedBandBeatEmphasis::workspace_bytes() const noexcept {return payloads(rows_,row_,acf_,rayleigh_,weights_,sorted_,result_)+average_.workspace_bytes()+correlation_.workspace_bytes();}
bool PreparedMovingThreshold::storage_bytes(std::size_t n,std::size_t h,std::size_t& b) noexcept {
    b=0;return n && h<=(SIZE_MAX-n)/2 && PreparedCausalAverage::storage_bytes(2*h+1,b) && add_float(b,n);
}
std::size_t PreparedMovingThreshold::workspace_bytes() const noexcept {return payload(values_)+average_.workspace_bytes();}
bool PreparedPeriodPath::storage_bytes(std::size_t n,PeriodPathConfig c,std::size_t& b) noexcept {
    b=0;if(!n || !period_geometry(c))return false;
    std::size_t temp=0,square=0,observations=0,half=0;
    if(!extent(std::floor(.1f*c.frame_rate),half) || !half || !PreparedMovingThreshold::storage_bytes(c.hop,8,b) || !PreparedCausalAverage::storage_bytes(2*half+1,temp))return false;
    return mul_bytes(c.hop,c.hop,square) && mul_bytes(n,c.hop,observations) && add_bytes(b,temp) && add_float(b,c.frame_length,2) && add_float(b,c.hop,5) &&
        add_float(b,square) && add_float(b,observations) && add_array(b,observations,sizeof(std::uint32_t)) && add_array(b,n,sizeof(std::uint32_t));
}
std::size_t PreparedPeriodPath::workspace_bytes() const noexcept {
    return payloads(frame_,acf_,comb_,filtered_,weights_,transition_,observations_,previous_,current_,predecessor_,path_)+threshold_.workspace_bytes()+average_.workspace_bytes();
}
bool PreparedRenewalBeatDecoder::storage_bytes(std::size_t samples,std::size_t blocks,RenewalBeatConfig c,std::size_t& b) noexcept {
    b=0;if(!samples || !blocks || c.upsampling<1 || c.upsampling>4 || !positive(c.frame_rate) || !positive(c.maximum_period_s) || !positive(c.sigma_ibi) || !positive(c.transition_mix) || c.transition_mix>=1)return false;
    PreparedRenewalBeatDecoder v;v.config_=c;v.rate_=c.frame_rate*float(c.upsampling);v.resolution_=1.f/v.rate_;
    std::size_t states=0,radius=0,models=0;
    if(!positive(v.rate_) || !v.state_count(c.maximum_period_s,states) || !extent(std::ceil(4.f*c.sigma_ibi/v.resolution_),radius) || radius>(SIZE_MAX-1)/2 || !mul_bytes(blocks,states,models))return false;
    return add_float(b,2*radius+1) && add_float(b,models,2) && add_float(b,states,2) && add_float(b,samples) && add_array(b,samples,2*sizeof(std::uint32_t)+sizeof(std::uint8_t));
}
std::size_t PreparedRenewalBeatDecoder::workspace_bytes() const noexcept {return payloads(gaussian_,reset_log_,advance_log_,previous_,current_,onset_,reset_from_,path_,beats_);}
bool PreparedPeriodBeatTracker::storage_bytes(std::size_t n,PeriodBeatTrackingConfig c,std::size_t& b) noexcept {
    b=0;std::size_t expanded=0,blocks=0,part=0,half=0;PeriodPathConfig p;dsp_kernels::TemporalWindowConfig w;
    if(!tracker_geometry(n,c,expanded,blocks,p,w) || !extent(std::floor(.1f*p.frame_rate),half) || !PreparedMovingThreshold::storage_bytes(expanded,half,b))return false;
    if(!PreparedCausalAverage::storage_bytes(2*half+1,part) || !add_bytes(b,part) || !PreparedPeriodPath::storage_bytes(blocks,p,part) || !add_bytes(b,part))return false;
    RenewalBeatConfig d{.frame_rate=c.frame_rate,.maximum_period_s=float(p.hop)/p.frame_rate,.upsampling=c.upsampling};
    return PreparedRenewalBeatDecoder::storage_bytes(expanded,blocks,d,part) && add_bytes(b,part) && dsp_kernels::PreparedTemporalWindow::storage_bytes(w,{},part) && add_bytes(b,part) &&
        add_float(b,n) && add_float(b,expanded,2) && add_float(b,blocks,2) && add_array(b,expanded,sizeof(std::uint8_t));
}
std::size_t PreparedPeriodBeatTracker::workspace_bytes() const noexcept {return payloads(normalized_,expanded_,thresholded_,periods_,ends_,mask_)+window_.workspace_bytes()+threshold_.workspace_bytes()+average_.workspace_bytes()+period_.workspace_bytes()+decoder_.workspace_bytes();}
bool PreparedBeatAgreement::storage_bytes(std::size_t methods,std::size_t beats,float minimum,std::size_t& b) noexcept {
    b=0;std::size_t pairs=0;return methods && beats && std::isfinite(minimum) && minimum>=0 && mul_bytes(methods,methods,pairs) && add_float(b,methods) && add_float(b,pairs) && add_float(b,128);
}
std::size_t PreparedBeatAgreement::workspace_bytes() const noexcept {return payloads(boundaries_,histogram_,scores_,pairwise_);}
bool PreparedMultiFeatureBeatTracker::storage_bytes(const std::array<std::size_t,5>& n,MultiFeaturePeriodConfig c,std::size_t& b) noexcept {
    b=0;std::size_t maximum=0;
    for(std::size_t i=0;i<5;++i){
        std::size_t part=0,count=0;PeriodBeatTrackingConfig d{c.frame_rates[i],c.minimum_bpm,c.maximum_bpm,c.upsampling[i],c.noise};
        if(!PreparedPeriodBeatTracker::storage_bytes(n[i],d,part) || !add_bytes(b,part) || !dsp_kernels::endpoint_expansion_size(n[i],c.upsampling[i],count) || !add_array(b,count,sizeof(float)+sizeof(std::uint8_t)))return false;
        maximum=std::max(maximum,count);
    }
    std::size_t part=0;return PreparedBeatAgreement::storage_bytes(5,maximum,c.agreement_minimum_seconds,part) && add_bytes(b,part);
}
std::size_t PreparedMultiFeatureBeatTracker::workspace_bytes() const noexcept {
    std::size_t b=agreement_.workspace_bytes();for(std::size_t i=0;i<5;++i)b+=trackers_[i].workspace_bytes()+payload(masks_[i])+payload(times_[i]);return b;
}
bool PreparedGlobalOnsetRecipe::storage_bytes(std::size_t n,GlobalOnsetRecipeConfig c,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(c.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    b=0;std::size_t frames=0,part=0;
    if(!positive(c.sample_rate) || unsigned(c.method)>1 || !front(n,c.frame_size,c.hop,false,frames,b,factory))return false;
    const auto bins=c.frame_size/2+1;
    if(!add_float(b,bins,5) || !add_float(b,frames))return false;
    if(c.method==GlobalOnsetMethod::InformationGain){
        std::size_t first=0,stop=0;
        return extent(std::round(40.f*float(c.frame_size)/c.sample_rate),first) && extent(std::round(5000.f*float(c.frame_size)/c.sample_rate)+1.f,stop) && PreparedInformationGain::storage_bytes(bins,first,stop,part) && add_bytes(b,part);
    }
    ErbFilterBankConfig erb;erb.sample_rate=c.band_sample_rate;erb.high_hz=c.sample_rate*.5f;
    return PreparedPolarNovelty::storage_bytes(bins,PolarNovelty::BeatEmphasis,part) && add_bytes(b,part) && PreparedErbFilterBank::storage_bytes(bins,erb,part) && add_bytes(b,part) &&
        PreparedBandBeatEmphasis::storage_bytes(frames,{c.sample_rate/float(c.hop),40},part,factory) && add_bytes(b,part) && add_float(b,40);
}
std::size_t PreparedGlobalOnsetRecipe::workspace_bytes() const noexcept {return payloads(complex_,magnitude_,phase_,distances_,curve_,erb_values_)+spectrum_.workspace_bytes()+gain_.workspace_bytes()+novelty_.workspace_bytes()+erb_.workspace_bytes()+emphasis_.workspace_bytes();}
namespace {
bool pcm_onset_storage(std::size_t frames,unsigned methods,std::size_t& b) noexcept {
    std::size_t part=0;
    constexpr SpectralOnsetMethod types[]{SpectralOnsetMethod::Complex,SpectralOnsetMethod::Rms,SpectralOnsetMethod::MelFlux};
    if(!add_float(b,1025,4) || !add_float(b,frames,methods))return false;
    for(unsigned i=0;i<methods;++i)if(!PreparedSpectralOnset::storage_bytes(1025,{types[i],44100},part) || !add_bytes(b,part))return false;
    return true;
}
bool pcm_single_storage(std::size_t frames,PcmBeatRecipeConfig c,std::size_t& b) noexcept {
    PeriodBeatTrackingConfig t{44100.f/1024.f,float(c.minimum_bpm),float(c.maximum_bpm),2,c.noise};std::size_t count=0,part=0;
    return PreparedPeriodBeatTracker::storage_bytes(frames,t,part) && add_bytes(b,part) && dsp_kernels::endpoint_expansion_size(frames,2,count) && add_array(b,count,sizeof(float)+sizeof(std::uint8_t));
}
bool additional_pcm_frames(std::size_t n,std::size_t& more) noexcept {
    dsp_kernels::TemporalWindowConfig w;w.length=2048;w.hop=512;w.center=false;w.tail=dsp_kernels::TemporalTail::CoverEnd;
    return dsp_kernels::PreparedTemporalWindow::frame_count(w,n,more);
}
bool pcm_multi_storage(std::size_t n,std::size_t frames,PcmBeatRecipeConfig c,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    std::size_t part=0,more=0;
    GlobalOnsetRecipeConfig global;global.window_precision=c.window_precision;global.method=GlobalOnsetMethod::BeatEmphasis;
    if(!PreparedGlobalOnsetRecipe::storage_bytes(n,global,part,factory) || !add_bytes(b,part))return false;
    global.method=GlobalOnsetMethod::InformationGain;if(!PreparedGlobalOnsetRecipe::storage_bytes(n,global,part,factory) || !add_bytes(b,part))return false;
    if(!additional_pcm_frames(n,more))return false;
    MultiFeaturePeriodConfig m;m.minimum_bpm=float(c.minimum_bpm);m.maximum_bpm=float(c.maximum_bpm);m.noise=c.noise;
    return PreparedMultiFeatureBeatTracker::storage_bytes({frames,frames,frames,more,more},m,part) && add_bytes(b,part);
}
}
bool PreparedPcmBeatRecipe::storage_bytes(std::size_t n,PcmBeatRecipeConfig c,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(c.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    b=0;std::size_t frames=0;
    if(unsigned(c.method)>1 || c.minimum_bpm<40 || c.minimum_bpm>180 || c.maximum_bpm<60 || c.maximum_bpm>250 || c.maximum_bpm<c.minimum_bpm+20 || !front(n,2048,1024,false,frames,b,factory))return false;
    const unsigned methods=c.method==PcmBeatMethod::SingleComplex?1:3;
    if(!pcm_onset_storage(frames,methods,b))return false;
    if(methods==1)return pcm_single_storage(frames,c,b);
    return pcm_multi_storage(n,frames,c,b,factory);
}
std::size_t PreparedPcmBeatRecipe::workspace_bytes() const noexcept {
    std::size_t b=payloads(complex_,magnitude_,phase_,ticks_,mask_)+spectrum_.workspace_bytes()+emphasis_.workspace_bytes()+gain_.workspace_bytes()+single_.workspace_bytes()+multi_.workspace_bytes();
    for(std::size_t i=0;i<3;++i)b+=payload(curves_[i])+spectral_[i].workspace_bytes();return b;
}
bool PreparedLagTempoVote::storage_bytes(std::size_t n,LagTempoVoteConfig c,std::size_t& b) noexcept {
    b=0;std::size_t bins=0;
    return n && c.audio_hop && positive(c.sample_rate) && positive(c.maximum_bpm) && c.maximum_bpm<=std::nextafter(float(INT_MAX),0.f) && extent(c.maximum_bpm/2.f+.5f,bins) && add_float(b,n,2) && add_array(b,bins+1,sizeof(std::uint64_t));
}
std::size_t PreparedLagTempoVote::workspace_bytes() const noexcept {return payloads(bpms_,estimates_,counts_);}
} // namespace rhythm_reference_ports
