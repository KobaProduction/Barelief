#include <dsp_kernels/onset_fusion.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
namespace {
bool valid_observation(const OnsetObservation& value,double observed,double ready) noexcept {
    if(value.pending_t_obs && (!std::isfinite(*value.pending_t_obs) || *value.pending_t_obs<0 || *value.pending_t_obs>observed))return false;
    if(!value.peak)return true;
    const auto& peak=*value.peak;
    return std::isfinite(peak.t_obs) && peak.t_obs>=0 && peak.t_obs<=observed
        && std::isfinite(peak.t_ready) && peak.t_ready>=peak.t_obs && peak.t_ready<=ready
        && std::isfinite(peak.strength) && peak.strength>=0 && peak.strength<=1
        && std::isfinite(peak.positive_mass) && peak.positive_mass>=0;
}
double observed_watermark(std::span<const OnsetObservation> values,double observed) noexcept {
    for(const auto& value:values)if(value.pending_t_obs)observed=std::min(observed,*value.pending_t_obs);
    return observed;
}
}
bool PreparedOnsetFusion::prepare(unsigned channels,double window) noexcept {
    if(!channels || channels>max_onset_channels || !std::isfinite(window) || window<0 || window>1)return false;
    channels_=channels;window_s_=channels==1?0:window;reset();return true;
}
void PreparedOnsetFusion::reset() noexcept {
    pending_count_=0;output_count_=0;dropped_=0;watermark_=0;last_time_.reset();last_peaks_.fill({});
}
bool PreparedOnsetFusion::valid(std::span<const OnsetObservation> values,double observed,double ready) const noexcept {
    if(!channels_ || values.size()!=channels_ || !std::isfinite(observed) || observed<0 || !std::isfinite(ready) || ready<observed)return false;
    if(last_time_ && observed<=*last_time_)return false;
    for(unsigned i=0;i<channels_;++i)if(!valid_channel(values[i],i,observed,ready))return false;
    return true;
}
bool PreparedOnsetFusion::valid_channel(const OnsetObservation& value,unsigned channel,double observed,double ready) const noexcept {
    if(!valid_observation(value,observed,ready))return false;
    if(value.pending_t_obs && *value.pending_t_obs<watermark_)return false;
    if(!value.peak)return true;
    if(value.peak->t_obs<watermark_)return false;
    return !last_peaks_[channel] || value.peak->t_obs>*last_peaks_[channel];
}
void PreparedOnsetFusion::append(std::span<const OnsetObservation> values) noexcept {
    for(unsigned i=0;i<channels_;++i)if(values[i].peak){
        pending_[pending_count_++]={*values[i].peak,std::uint16_t(1U<<i)};
        last_peaks_[i]=values[i].peak->t_obs;
    }
    std::sort(pending_.begin(),pending_.begin()+std::ptrdiff_t(pending_count_),[](const auto& a,const auto& b){
        return a.peak.t_obs<b.peak.t_obs || (a.peak.t_obs==b.peak.t_obs && a.channels<b.channels);
    });
}
std::size_t PreparedOnsetFusion::group(ConsolidatedOnset& value) const noexcept {
    value=pending_[0];std::size_t count=1;
    while(count<pending_count_){
        const auto& next=pending_[count];
        if(next.peak.t_obs-value.peak.t_obs>window_s_ || (next.channels&value.channels))break;
        value.channels|=next.channels;
        value.peak.strength=std::max(value.peak.strength,next.peak.strength);
        value.peak.positive_mass=std::max(value.peak.positive_mass,next.peak.positive_mass);++count;
    }
    return count;
}
void PreparedOnsetFusion::drain(double watermark,double ready) noexcept {
    while(pending_count_ && pending_[0].peak.t_obs+window_s_<watermark){
        ConsolidatedOnset value;const auto count=group(value);value.peak.t_ready=ready;
        output_[output_count_++]=value;
        std::move(pending_.begin()+std::ptrdiff_t(count),pending_.begin()+std::ptrdiff_t(pending_count_),pending_.begin());
        pending_count_-=count;
    }
}
bool PreparedOnsetFusion::process(std::span<const OnsetObservation> values,double observed,double ready) noexcept {
    if(!valid(values,observed,ready))return false;
    output_count_=0;dropped_=0;last_time_=observed;
    const auto incoming=std::count_if(values.begin(),values.end(),[](const auto& value){return value.peak.has_value();});
    if(pending_count_+std::size_t(incoming)>max_pending_onsets){
        dropped_=unsigned(pending_count_)+unsigned(incoming);pending_count_=0;return true;
    }
    append(values);const double watermark=observed_watermark(values,observed);
    drain(watermark,ready);watermark_=watermark;return true;
}
}
