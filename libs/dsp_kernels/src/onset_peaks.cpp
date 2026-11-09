// Adapted from librosa 1.0.0 util/utils.py peak_pick and onset.py onset_backtrack.
// ISC, Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
// Source: librosa/librosa@3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/onset_peaks.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
bool energy_minimum(std::span<const float> energy,std::size_t at) noexcept {
    return energy[at]<=energy[at-1] && energy[at]<energy[at+1];
}
bool overlap(std::span<const std::size_t> input,std::span<std::size_t> output) noexcept {
    if(input.empty())return false;
    const std::less<const std::size_t*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
}
bool onset_detection_parameters(float rate,OnsetPeakConfig& output) noexcept {
    if(!std::isfinite(rate) || rate<=0)return false;
    const float average=std::floor(.1f*rate),maximum=std::floor(.03f*rate);
    if(!std::isfinite(average) || average>=float(SIZE_MAX))return false;
    OnsetPeakConfig config;config.pre_max=std::size_t(maximum);config.post_max=1;
    config.pre_average=std::size_t(average);config.post_average=config.pre_average+1;config.wait=config.pre_max;
    config.normalize=true;config.suppress_empty=true;output=config;return true;
}
void PreparedOnsetPeakPicker::allocate(std::size_t frames,OnsetPeakConfig config) {
    config_=config;maximum_frames_=frames;
    values_.resize(frames);prefix_.resize(frames+1);scores_.resize(frames+1);
    links_.resize(frames+1);counts_.resize(frames+1);taken_.resize(frames);mask_.resize(frames);
}
bool PreparedOnsetPeakPicker::prepare(std::size_t frames,OnsetPeakConfig config) {
    std::size_t required=0;if(!storage_bytes(frames,config,required))return false;
    if(!frames || frames==SIZE_MAX || !config.post_max || !config.post_average || !std::isfinite(config.delta) || config.delta<0)return false;
    if(config.selection<PeakSelection::Greedy || config.selection>PeakSelection::MaximumValue)return false;
    PreparedOnsetPeakPicker next;
    next.allocate(frames,config);
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedOnsetPeakPicker::normalize(std::span<const float> input) noexcept {
    const auto [low,high]=std::minmax_element(input.begin(),input.end());
    const float range=*high-*low;
    if(!std::isfinite(range))return false;
    for(std::size_t i=0;i<input.size();++i)values_[i]=(input[i]-*low)/(range+std::numeric_limits<float>::min());
    return true;
}
void PreparedOnsetPeakPicker::pick(std::size_t count) noexcept {
    if(config_.suppress_empty && !std::any_of(values_.begin(),values_.begin()+std::ptrdiff_t(count),[](float value){return value!=0;}))return;
    if(config_.selection==PeakSelection::Greedy)greedy(count);else optimal(count);
}
bool PreparedOnsetPeakPicker::accumulate_prefix(std::size_t count) noexcept {
    prefix_[0]=0;
    for(std::size_t i=0;i<count;++i){
        prefix_[i+1]=prefix_[i]+values_[i];
        if(!std::isfinite(prefix_[i+1]))return false;
    }
    return true;
}
bool PreparedOnsetPeakPicker::load(std::span<const float> input) noexcept {
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(input.begin(),input.end(),values_.begin());
    if(config_.normalize && !input.empty() && !normalize(input))return false;
    if(config_.selection==PeakSelection::Greedy)return true;
    return accumulate_prefix(input.size());
}
bool PreparedOnsetPeakPicker::candidate(std::size_t i,std::size_t count) const noexcept {
    const auto left=i>config_.pre_max?i-config_.pre_max:0,right=i+std::min(config_.post_max,count-i);
    if(values_[i]!=*std::max_element(values_.begin()+std::ptrdiff_t(left),values_.begin()+std::ptrdiff_t(right)))return false;
    const auto first=i>config_.pre_average?i-config_.pre_average:0,last=i+std::min(config_.post_average,count-i);
    const float mean=average(first,last);
    return values_[i]>=mean+config_.delta;
}
float PreparedOnsetPeakPicker::average(std::size_t first,std::size_t last) const noexcept {
    if(config_.selection!=PeakSelection::Greedy)return (prefix_[last]-prefix_[first])/float(last-first);
    float sum=0;
    for(auto i=first;i<last;++i)sum+=values_[i];
    return sum/float(last-first);
}
std::size_t PreparedOnsetPeakPicker::next(std::size_t index,std::size_t count) const noexcept {
    return index+std::min(count-index,config_.wait>=count?count:config_.wait+1);
}
void PreparedOnsetPeakPicker::greedy(std::size_t count) noexcept {
    std::size_t i=0;
    while(i<count){if(candidate(i,count)){mask_[i]=1;i=next(i,count);}else ++i;}
}
bool PreparedOnsetPeakPicker::select(std::size_t index,std::size_t following) noexcept {
    if(config_.selection==PeakSelection::MaximumCount){
        const auto value=counts_[following]+1;
        if(value<=counts_[index])return false;
        counts_[index]=value;return true;
    }
    const float value=values_[index]+scores_[following];
    if(value<=scores_[index])return false;
    scores_[index]=value;return true;
}
void PreparedOnsetPeakPicker::inherit_selection(std::size_t index) noexcept {
    scores_[index]=scores_[index+1];counts_[index]=counts_[index+1];
    links_[index]=index+1;taken_[index]=0;
}
void PreparedOnsetPeakPicker::optimal(std::size_t count) noexcept {
    scores_[count]=0;counts_[count]=0;links_[count]=count;
    for(std::size_t i=count;i-->0;){
        inherit_selection(i);
        if(!candidate(i,count))continue;
        const auto following=next(i,count);
        if(select(i,following)){links_[i]=following;taken_[i]=1;}
    }
    for(std::size_t i=0;i<count;i=links_[i])mask_[i]=taken_[i];
}
bool PreparedOnsetPeakPicker::process(std::span<const float> input,std::span<std::uint8_t> output) noexcept {
    result_frames_=0;
    if(!maximum_frames_ || input.size()>maximum_frames_ || output.size()!=input.size() || !load(input))return false;
    std::fill_n(mask_.begin(),input.size(),std::uint8_t{0});
    pick(input.size());
    std::copy_n(mask_.begin(),input.size(),output.begin());result_frames_=input.size();return true;
}
bool PreparedOnsetBacktracker::prepare(std::size_t frames) {
    std::size_t required=0;if(!storage_bytes(frames,required))return false;
    if(!frames)return false;
    PreparedOnsetBacktracker next;next.maximum_frames_=frames;next.minima_.resize(frames);if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
std::size_t PreparedOnsetBacktracker::collect_minima(std::span<const float> energy) noexcept {
    std::size_t count=1;
    minima_[0]=0;
    for(std::size_t i=1;i+1<energy.size();++i)if(energy_minimum(energy,i))minima_[count++]=i;
    return count;
}
bool PreparedOnsetBacktracker::process(std::span<const std::size_t> events,std::span<const float> energy,std::span<std::size_t> output) noexcept {
    if(!maximum_frames_ || energy.size()>maximum_frames_ || events.size()!=output.size() || overlap(events,output))return false;
    if(!std::all_of(energy.begin(),energy.end(),[](float x){return std::isfinite(x);}))return false;
    // Events may lie beyond the supplied energy curve; source left matching
    // still selects its final preceding minimum (or the frame-zero anchor).
    const auto count=collect_minima(energy);
    for(std::size_t i=0;i<events.size();++i){
        const auto right=std::upper_bound(minima_.begin(),minima_.begin()+std::ptrdiff_t(count),events[i]);
        output[i]=*(right-1);
    }
    return true;
}
namespace {
bool masks_overlap(std::span<const std::uint8_t> input,std::span<std::uint8_t> output) noexcept {
    if(input.empty())return false;
    const std::less<const std::uint8_t*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
}
namespace {
void backtrack_mask(std::span<const std::uint8_t> input,std::span<const float> energy,std::span<std::uint8_t> output) noexcept {
    std::fill(output.begin(),output.end(),std::uint8_t{0});
    std::size_t minimum=0;
    for(std::size_t i=0;i<input.size();++i){
        if(i && i+1<energy.size() && energy_minimum(energy,i))minimum=i;
        if(input[i])output[minimum]=1;
    }
}
}
bool backtrack_onset_peaks(std::span<const std::uint8_t> input,std::span<const float> energy,std::span<std::uint8_t> output) noexcept {
    if(input.size()!=energy.size() || input.size()!=output.size())return false;
    if(masks_overlap(input,output))return false;
    if(!std::all_of(energy.begin(),energy.end(),[](float x){return std::isfinite(x);}))return false;
    backtrack_mask(input,energy,output);
    return true;
}
}


namespace dsp_kernels {
bool PreparedOnsetPeakPicker::storage_bytes(std::size_t n,OnsetPeakConfig c,std::size_t& out) noexcept {
    if(!n || n==SIZE_MAX || !c.post_max || !c.post_average || !std::isfinite(c.delta) || c.delta<0 ||
       unsigned(c.selection)>unsigned(PeakSelection::MaximumValue))return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,n,sizeof(float)+2*sizeof(std::uint8_t)) ||
       !prepared_storage::add_array(bytes,n+1,2*sizeof(float)+2*sizeof(std::size_t)))return false;
    out=bytes;return true;
}
std::size_t PreparedOnsetPeakPicker::workspace_bytes() const noexcept {
    return (values_.capacity()+prefix_.capacity()+scores_.capacity())*sizeof(float)+
        (links_.capacity()+counts_.capacity())*sizeof(std::size_t)+taken_.capacity()+mask_.capacity();
}
bool PreparedOnsetBacktracker::storage_bytes(std::size_t n,std::size_t& out) noexcept {
    return n && prepared_storage::mul_bytes(n,sizeof(std::size_t),out);
}
}
