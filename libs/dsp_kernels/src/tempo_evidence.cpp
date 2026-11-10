#include <dsp_kernels/tempo_evidence.h>
#include <dsp_kernels/peaks.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels::rhythm {
namespace {
bool valid_size(std::size_t size) noexcept {return size>=32 && size<=16384 && !(size&(size-1));}
bool valid_config(const TempoEvidenceConfig& c) noexcept {
    return c.frame_rate_fps>=10 && c.frame_rate_fps<=1000 && valid_size(c.short_fft) && valid_size(c.long_fft) &&
        c.short_fft<=c.long_fft && std::isfinite(c.min_bpm) && std::isfinite(c.max_bpm) &&
        c.min_bpm>=20 && c.max_bpm>c.min_bpm && c.max_bpm<30*c.frame_rate_fps &&
        std::isfinite(c.min_observation_s) && std::isfinite(c.mature_observation_s) &&
        c.min_observation_s>0 && c.min_observation_s<=c.mature_observation_s;
}
bool valid_curve(const PeriodicityCurve& curve,std::size_t size) noexcept {
    if((curve.fourier.size()!=size/2 && curve.fourier.size()!=size/2+1) ||
       curve.autocorrelation.size()<2 || curve.autocorrelation.size()>size || curve.filled>size)return false;
    return std::all_of(curve.fourier.begin(),curve.fourier.end(),[](float v){return std::isfinite(v) && v>=0;}) &&
        std::all_of(curve.autocorrelation.begin(),curve.autocorrelation.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1;});
}
float sample(std::span<const float> values,float index) noexcept {
    if(index<0 || index>float(values.size()-1))return 0;
    const auto left=std::size_t(index),right=std::min(left+1,values.size()-1);
    const auto fraction=index-float(left);
    return (1-fraction)*values[left]+fraction*values[right];
}
float offset(std::span<const float> values,std::size_t index,bool logarithmic) noexcept {
    const float left=values[index-1],center=values[index],right=values[index+1];
    if(logarithmic && left>0 && center>0 && right>0) {
        const auto relative_left=std::log(double(left)/double(center));
        const auto relative_right=std::log(double(right)/double(center));
        return quadratic_peak_offset(relative_left,0.,relative_right);
    }
    return quadratic_peak_offset(left,center,right);
}
struct Peak {float bpm=0,strength=0;};
struct Peaks {
    std::array<Peak,8> values{};std::size_t count=0;
    void include(Peak peak) noexcept {
        if(count==values.size() && peak.strength<=values.back().strength)return;
        auto at=std::min(count,values.size()-1);
        if(count<values.size())++count;
        while(at && peak.strength>values[at-1].strength){values[at]=values[at-1];--at;}
        values[at]=peak;
    }
};
Peaks peaks(std::span<const float> values,const TempoEvidenceConfig& c,std::size_t size,bool fourier) noexcept {
    Peaks result;
    for(std::size_t i=1;i+1<values.size();++i){
        if(values[i]<=0 || values[i]<values[i-1] || values[i]<=values[i+1])continue;
        const auto position=float(i)+offset(values,i,fourier);
        const auto bpm=fourier?position*60*c.frame_rate_fps/float(size):60*c.frame_rate_fps/position;
        if(bpm>=c.min_bpm && bpm<=c.max_bpm)result.include({bpm,values[i]});
    }
    return result;
}
float maximum(const PeriodicityCurve& curve,const TempoEvidenceConfig& c,std::size_t size) noexcept {
    float value=0;
    for(std::size_t i=1;i<curve.fourier.size();++i){
        const auto bpm=float(i)*60*c.frame_rate_fps/float(size);
        if(bpm>=c.min_bpm && bpm<=c.max_bpm)value=std::max(value,curve.fourier[i]);
    }
    return value;
}
float maturity(const PeriodicityCurve& curve,const TempoEvidenceConfig& c) noexcept {
    const auto seconds=float(curve.filled)/c.frame_rate_fps;
    return seconds<c.min_observation_s?0:std::min(1.f,seconds/c.mature_observation_s);
}
struct Support {float fourier=0,acf=0;};
Support support(const PeriodicityCurve& curve,const TempoEvidenceConfig& c,std::size_t size,float bpm,float highest) noexcept {
    const auto fourier=highest>0?sample(curve.fourier,bpm*float(size)/(60*c.frame_rate_fps))/highest:0;
    return {std::clamp(fourier,0.f,1.f),std::max(0.f,sample(curve.autocorrelation,60*c.frame_rate_fps/bpm))};
}
bool better(const TempoEvidenceCandidate& a,const TempoEvidenceCandidate& b) noexcept {
    return a.score>b.score || (a.score==b.score && a.bpm<b.bpm);
}
std::size_t matching_slot(const TempoEvidenceOutput& output,float bpm) noexcept {
    for(std::size_t i=0;i<output.count;++i)
        if(std::abs(bpm-output.candidates[i].bpm)/output.candidates[i].bpm<.015f)return i;
    return output.count;
}
bool merge_origins(TempoEvidenceCandidate& current,TempoEvidenceCandidate& candidate) noexcept {
    candidate.origin_curves|=current.origin_curves;
    if(better(candidate,current))return true;
    current.origin_curves=candidate.origin_curves;return false;
}
void retain(TempoEvidenceOutput& output,TempoEvidenceCandidate candidate) noexcept {
    auto at=matching_slot(output,candidate.bpm);
    if(at<output.count && !merge_origins(output.candidates[at],candidate))return;
    if(at==output.count){
        if(at<output.candidates.size())++output.count;
        else {at=output.count-1;if(!better(candidate,output.candidates[at]))return;}
    }
    output.candidates[at]=candidate;
    std::sort(output.candidates.begin(),output.candidates.begin()+std::ptrdiff_t(output.count),better);
}
bool related(float a,float b) noexcept {
    const auto ratio=std::max(a,b)/std::min(a,b);
    return std::abs(ratio/2-1)<.02f || std::abs(ratio/3-1)<.02f ||
        std::abs(ratio/4-1)<.02f || std::abs(ratio/1.5f-1)<.02f;
}
bool ambiguous(const TempoEvidenceOutput& output) noexcept {
    if(!output.count)return false;
    for(std::size_t i=0;i<output.count;++i)for(std::size_t j=i+1;j<output.count;++j)
        if(output.candidates[j].score>=.5f*output.candidates[0].score && related(output.candidates[i].bpm,output.candidates[j].bpm))return true;
    return false;
}
TempoEvidenceCandidate candidate(const PeriodicityCurve& s,const PeriodicityCurve& l,const TempoEvidenceConfig& c,
                                 float bpm,float smax,float lmax) noexcept {
    const auto short_support=support(s,c,c.short_fft,bpm,smax),long_support=support(l,c,c.long_fft,bpm,lmax);
    const auto sw=maturity(s,c),lw=maturity(l,c);
    const auto score=.5f*((short_support.fourier+short_support.acf)*sw+(long_support.fourier+long_support.acf)*lw)/(sw+lw)*std::max(sw,lw);
    return {bpm,score,short_support.fourier,long_support.fourier,short_support.acf,long_support.acf,0};
}
}
bool PreparedTempoEvidence::prepare(const TempoEvidenceConfig& config) noexcept {
    if(!valid_config(config))return false;
    config_=config;output_={};proposal_count_=0;prepared_=true;return true;
}
void PreparedTempoEvidence::propose(float bpm,std::uint8_t origin) noexcept {
    for(const float factor:{.25f,1.f/3,.5f,2.f/3,1.f,1.5f,2.f,3.f,4.f}){
        const auto value=bpm*factor;
        if(value<config_.min_bpm || value>config_.max_bpm)continue;
        if(proposal_count_==proposals_.size()){++output_.truncated_proposals;continue;}
        proposals_[proposal_count_++]={value,origin};
    }
}
void PreparedTempoEvidence::collect(const PeriodicityCurve& curve,std::size_t size,std::uint8_t bit) noexcept {
    if(!maturity(curve,config_))return;
    const auto f=peaks(curve.fourier,config_,size,true),a=peaks(curve.autocorrelation,config_,size,false);
    for(std::size_t i=0;i<f.count;++i)propose(f.values[i].bpm,bit);
    for(std::size_t i=0;i<a.count;++i)propose(a.values[i].bpm,std::uint8_t(bit<<2));
}
bool PreparedTempoEvidence::process(const PeriodicityCurve& s,const PeriodicityCurve& l) noexcept {
    if(!prepared_ || !valid_curve(s,config_.short_fft) || !valid_curve(l,config_.long_fft))return false;
    output_={};proposal_count_=0;output_.maturity=std::max(maturity(s,config_),maturity(l,config_));
    if(!output_.maturity)return true;
    collect(s,config_.short_fft,1);collect(l,config_.long_fft,2);
    rank(s,l);return true;
}
void PreparedTempoEvidence::rank(const PeriodicityCurve& s,const PeriodicityCurve& l) noexcept {
    const auto smax=maximum(s,config_,config_.short_fft),lmax=maximum(l,config_,config_.long_fft);
    for(std::size_t i=0;i<proposal_count_;++i){
        auto value=candidate(s,l,config_,proposals_[i].bpm,smax,lmax);value.origin_curves=proposals_[i].origin;
        if(value.score>0)retain(output_,value);
    }
    output_.proposals=proposal_count_;output_.harmonic_ambiguity=ambiguous(output_);
}
}
