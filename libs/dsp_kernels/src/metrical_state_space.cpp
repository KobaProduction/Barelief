// Adapted from madmom features/beats_hmm.py at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/metrical_state_space.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
std::uint32_t round_even(float value) noexcept {
    const float low=std::floor(value),fraction=value-low;
    const auto integer=std::uint32_t(low);
    return integer+std::uint32_t(fraction>.5f || (fraction==.5f && integer%2));
}
void log_grid(float low,float high,std::size_t count,std::vector<std::uint32_t>& values) {
    values.clear();
    for(std::size_t i=0;i<count;++i){
        const float position=count>1?float(i)/float(count-1):0;
        const auto period=round_even(std::exp2(std::log2(low)+position*(std::log2(high)-std::log2(low))));
        if(values.empty() || values.back()!=period)values.push_back(period);
    }
}
}
namespace {
bool requested_log_grid(float low,float high,std::size_t requested,std::vector<std::uint32_t>& values) {
    auto count=requested;
    do{
        if(count==std::size_t(UINT32_MAX))return false;
        log_grid(low,high,count++,values);
    }while(values.size()<requested);
    return true;
}
bool normalize_period_row(std::span<float> output,float sum,bool fixed_interval) noexcept {
    if(fixed_interval)return true;
    if(sum<=0 || !std::isfinite(sum))return false;
    for(auto& probability:output)probability/=sum;
    return true;
}
bool period_transition_row(std::uint32_t from,std::size_t origin,std::span<const std::uint32_t> to,TempoTransitionRule rule,std::span<float> output,float threshold) noexcept {
    float sum=0;
    for(std::size_t target=0;target<to.size();++target){
        float p=rule.fixed_interval?float(origin==target):std::exp(-rule.lambda*std::abs(float(to[target])/float(from)-1.f));
        if(!rule.fixed_interval && p<=threshold)p=0;
        output[target]=p;sum+=p;
    }
    return normalize_period_row(output,sum,rule.fixed_interval);
}
}
bool discrete_period_grid(float low,float high,std::size_t requested,std::vector<std::uint32_t>& output) {
    if(!std::isfinite(low) || !std::isfinite(high) || low<1 || high<low || high>=float(UINT32_MAX))return false;
    const auto minimum=round_even(low),maximum=round_even(high);
    std::vector<std::uint32_t> values;
    values.reserve(std::size_t(maximum-minimum)+1);
    if(!requested || requested>=std::size_t(maximum-minimum)+1){
        for(auto period=minimum;;++period){values.push_back(period);if(period==maximum)break;}
    }else if(!requested_log_grid(low,high,requested,values))return false;
    output=std::move(values);return true;
}
bool exponential_period_transitions(std::span<const std::uint32_t> from,std::span<const std::uint32_t> to,
                                    TempoTransitionRule rule,std::span<float> output,float threshold) noexcept {
    if(from.empty() || to.empty() || from.size()>std::numeric_limits<std::size_t>::max()/to.size() ||
       output.size()!=from.size()*to.size() || !std::isfinite(rule.lambda) ||
       !std::isfinite(threshold) || threshold<0 || threshold>=1 ||
       std::find(from.begin(),from.end(),0)!=from.end() || std::find(to.begin(),to.end(),0)!=to.end())return false;
    for(std::size_t origin=0;origin<from.size();++origin)
        if(!period_transition_row(from[origin],origin,to,rule,output.subspan(origin*to.size(),to.size()),threshold))return false;
    return true;
}
bool PreparedMetricalStateSpace::beat(Pattern& pattern,std::size_t beat_number,std::size_t pattern_number) {
    for(const auto period:pattern.periods){
        if(positions_.size()>std::size_t(UINT32_MAX)-period)return false;
        pattern.first.push_back(std::uint32_t(positions_.size()));
        for(std::uint32_t phase=0;phase<period;++phase){
            positions_.push_back(float(beat_number)+float(phase)/float(period));
            state_periods_.push_back(period);state_patterns_.push_back(std::uint32_t(pattern_number));
        }
        pattern.last.push_back(std::uint32_t(positions_.size()-1));
    }
    return true;
}
bool PreparedMetricalStateSpace::append_pattern_beats(Pattern& pattern,std::size_t index) {
    if(pattern.beats>SIZE_MAX/pattern.periods.size())return false;
    pattern.first.reserve(pattern.beats*pattern.periods.size());pattern.last.reserve(pattern.beats*pattern.periods.size());
    for(std::size_t beat_number=0;beat_number<pattern.beats;++beat_number)
        if(!beat(pattern,beat_number,index))return false;
    return true;
}
void PreparedMetricalStateSpace::reserve_states(std::size_t patterns,std::size_t states) {
    patterns_.reserve(patterns);positions_.reserve(states);
    state_periods_.reserve(states);state_patterns_.reserve(states);
}
bool PreparedMetricalStateSpace::pattern(MetricalPatternConfig config,std::size_t index) {
    if(!config.beats)return false;
    Pattern pattern;pattern.beats=config.beats;
    if(!discrete_period_grid(config.minimum_interval,config.maximum_interval,config.num_intervals,pattern.periods))return false;
    if(!append_pattern_beats(pattern,index))return false;
    patterns_.push_back(std::move(pattern));return true;
}
bool PreparedMetricalStateSpace::prepare(std::span<const MetricalPatternConfig> configs) {
    PreparedMetricalStateSpace next;
    std::size_t bytes=0,states=0,edges=0;
    if(!storage_bytes(configs,bytes) || !geometry(configs,states,edges))return false;
    next.reserve_states(configs.size(),states);
    for(std::size_t i=0;i<configs.size();++i)if(!next.pattern(configs[i],i))return false;
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
std::size_t PreparedMetricalStateSpace::beats(std::size_t p) const noexcept {return p<patterns_.size()?patterns_[p].beats:0;}
std::span<const std::uint32_t> PreparedMetricalStateSpace::periods(std::size_t p) const noexcept {
    return p<patterns_.size()?std::span<const std::uint32_t>(patterns_[p].periods):std::span<const std::uint32_t>{};
}
std::span<const std::uint32_t> PreparedMetricalStateSpace::first_states(std::size_t p,std::size_t b) const noexcept {
    if(p>=patterns_.size() || b>=patterns_[p].beats)return {};
    const auto n=patterns_[p].periods.size();return std::span<const std::uint32_t>(patterns_[p].first).subspan(b*n,n);
}
std::span<const std::uint32_t> PreparedMetricalStateSpace::last_states(std::size_t p,std::size_t b) const noexcept {
    if(p>=patterns_.size() || b>=patterns_[p].beats)return {};
    const auto n=patterns_[p].periods.size();return std::span<const std::uint32_t>(patterns_[p].last).subspan(b*n,n);
}
}

namespace dsp_kernels {
bool period_grid_bound(float low,float high,std::size_t& count,std::size_t& sum) noexcept {
    if(!std::isfinite(low) || !std::isfinite(high) || low<1 || high<low || high>=float(UINT32_MAX))return false;
    const auto first=round_even(low),last=round_even(high);count=std::size_t(last-first)+1;
    std::size_t paired=first,n=count;if(!prepared_storage::add_bytes(paired,last))return false;
    if(n%2==0)n/=2;else paired/=2;
    return prepared_storage::mul_bytes(n,paired,sum);
}
bool PreparedMetricalStateSpace::geometry(std::span<const MetricalPatternConfig> configs,std::size_t& states,std::size_t& transitions) noexcept {
    if(configs.empty() || configs.size()>UINT32_MAX)return false;
    std::size_t total=0,edges=0;
    for(const auto& c:configs){
        std::size_t count=0,sum=0,weighted=0,square=0;
        if(!c.beats || !period_grid_bound(c.minimum_interval,c.maximum_interval,count,sum) ||
           !prepared_storage::mul_bytes(sum,c.beats,weighted) || !prepared_storage::add_bytes(total,weighted) ||
           !prepared_storage::mul_bytes(count,count,square) || !prepared_storage::mul_bytes(square,c.beats,weighted) ||
           !prepared_storage::add_bytes(edges,weighted) || !prepared_storage::mul_bytes(sum-count,c.beats,weighted) ||
           !prepared_storage::add_bytes(edges,weighted))return false;
    }
    if(!total || total>UINT32_MAX)return false;
    states=total;transitions=edges;return true;
}
bool PreparedMetricalStateSpace::storage_bytes(std::span<const MetricalPatternConfig> configs,std::size_t& out) noexcept {
    std::size_t states=0,edges=0,bytes=0;
    if(!geometry(configs,states,edges) || !prepared_storage::add_array(bytes,configs.size(),sizeof(Pattern)) ||
       !prepared_storage::add_array(bytes,states,sizeof(float)+2*sizeof(std::uint32_t)))return false;
    for(const auto& c:configs){
        std::size_t count=0,sum=0,first=0;
        if(!period_grid_bound(c.minimum_interval,c.maximum_interval,count,sum) || !prepared_storage::mul_bytes(count,c.beats,first) ||
           !prepared_storage::add_array(bytes,count,sizeof(std::uint32_t)) || !prepared_storage::add_array(bytes,first,2*sizeof(std::uint32_t)))return false;
    }
    out=bytes;return true;
}
std::size_t PreparedMetricalStateSpace::workspace_bytes() const noexcept {
    std::size_t bytes=patterns_.capacity()*sizeof(Pattern)+positions_.capacity()*sizeof(float)+
        (state_periods_.capacity()+state_patterns_.capacity())*sizeof(std::uint32_t);
    for(const auto& p:patterns_)bytes+=(p.periods.capacity()+p.first.capacity()+p.last.capacity())*sizeof(std::uint32_t);
    return bytes;
}
}
