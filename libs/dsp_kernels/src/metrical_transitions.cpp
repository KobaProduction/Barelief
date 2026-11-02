// Adapted from madmom features/beats_hmm.py at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/metrical_transitions.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
bool uniform_pattern_switches(std::size_t patterns,float probability,std::vector<float>& output) {
    if(!patterns || patterns>std::numeric_limits<std::size_t>::max()/patterns ||
       !std::isfinite(probability) || probability<0 || probability>1)return false;
    std::vector<float> values(patterns*patterns,patterns>1?probability/float(patterns-1):1.f);
    for(std::size_t p=0;p<patterns;++p)values[p*patterns+p]=patterns>1?1.f-probability:1.f;
    output=std::move(values);return true;
}
bool PreparedMetricalTransitions::boundary(const PreparedMetricalStateSpace& space,std::size_t p,std::size_t beat,TempoTransitionRule rule) {
    const auto periods=space.periods(p),to=space.first_states(p,beat);
    const auto from=space.last_states(p,beat?beat-1:space.beats(p)-1);
    if(periods.size()>std::numeric_limits<std::size_t>::max()/periods.size())return false;
    std::vector<float> probabilities(periods.size()*periods.size());
    if(!exponential_period_transitions(periods,periods,rule,probabilities))return false;
    for(std::size_t target=0;target<to.size();++target)
        for(std::size_t origin=0;origin<from.size();++origin){
            const float probability=probabilities[origin*to.size()+target];
            if(probability==0)continue;
            const MarkovTransition edge{from[origin],to[target],probability};
            edges_.push_back(edge);
            if(!beat){boundaries_[p].push_back(edge);boundary_indices_[p].push_back(edges_.size()-1);}
        }
    return true;
}
bool PreparedMetricalTransitions::pattern(const PreparedMetricalStateSpace& space,std::size_t p,std::span<const TempoTransitionRule> rules) {
    for(std::size_t beat=0;beat<space.beats(p);++beat){
        const auto first=space.first_states(p,beat),last=space.last_states(p,beat);
        for(std::size_t interval=0;interval<first.size();++interval)
            for(auto state=first[interval]+1;state<=last[interval];++state)edges_.push_back({state-1,state,1});
        if(!boundary(space,p,beat,rules[beat]))return false;
    }
    return true;
}
bool PreparedMetricalTransitions::switches(std::size_t target,std::size_t origin,float probability) {
    const auto& to=boundaries_[target];const auto& from=boundaries_[origin];
    if(to.size()!=from.size())return false;
    for(std::size_t edge=0;edge<to.size();++edge)
        edges_.push_back({from[edge].from,to[edge].to,to[edge].probability*probability});
    return true;
}
bool PreparedMetricalTransitions::switch_patterns(std::span<const float> matrix) {
    const auto n=boundaries_.size();
    if(matrix.empty() || n<2)return true;
    if(n>std::numeric_limits<std::size_t>::max()/n || matrix.size()!=n*n ||
       !std::all_of(matrix.begin(),matrix.end(),[](float p){return std::isfinite(p) && p>=0 && p<=1;}))return false;
    for(std::size_t p=0;p<n;++p)
        for(const auto index:boundary_indices_[p])edges_[index].probability*=matrix[p*n+p];
    for(std::size_t target=0;target<n;++target)
        for(std::size_t origin=0;origin<n;++origin)
            if(target!=origin && !switches(target,origin,matrix[target*n+origin]))return false;
    return true;
}
bool PreparedMetricalTransitions::prepare(const PreparedMetricalStateSpace& space,std::span<const TempoTransitionRule> rules,
                                         std::span<const float> switches) {
    if(!space.states())return false;
    std::size_t count=0;
    for(std::size_t p=0;p<space.patterns();++p){
        if(space.beats(p)>std::numeric_limits<std::size_t>::max()-count)return false;
        count+=space.beats(p);
    }
    if(rules.size()!=count)return false;
    std::size_t periods=0;for(std::size_t p=0;p<space.patterns();++p)periods=std::max(periods,space.periods(p).size());
    std::size_t bound=0,edges=0;
    if(!storage_bytes(space.states(),space.patterns(),count,periods,!switches.empty(),bound,edges))return false;
    PreparedMetricalTransitions next;next.edges_.reserve(edges);next.boundaries_.resize(space.patterns());
    next.boundary_indices_.resize(space.patterns());std::size_t offset=0;
    for(std::size_t p=0;p<space.patterns();++p){
        const auto square=space.periods(p).size()*space.periods(p).size();
        next.boundaries_[p].reserve(square);next.boundary_indices_[p].reserve(square);
    }
    for(std::size_t p=0;p<space.patterns();++p){
        if(!next.pattern(space,p,rules.subspan(offset,space.beats(p))))return false;
        offset+=space.beats(p);
    }
    if(!next.switch_patterns(switches))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedMetricalTransitions::storage_bytes(std::size_t states,std::size_t patterns,std::size_t beats,std::size_t periods,
    bool switches,std::size_t& out,std::size_t& maximum_edges) noexcept {
    std::size_t square=0,edges=states,part=0,bytes=0;
    if(!states || !patterns || !beats || !periods || !prepared_storage::mul_bytes(periods,periods,square) ||
       !prepared_storage::mul_bytes(beats,square,part) || !prepared_storage::add_bytes(edges,part))return false;
    if(switches && (!prepared_storage::mul_bytes(patterns,patterns-1,part) || !prepared_storage::mul_bytes(part,square,part) ||
       !prepared_storage::add_bytes(edges,part)))return false;
    if(!prepared_storage::add_array(bytes,edges,sizeof(MarkovTransition)) ||
       !prepared_storage::add_array(bytes,patterns,sizeof(std::vector<MarkovTransition>)+sizeof(std::vector<std::size_t>)) ||
       !prepared_storage::mul_bytes(patterns,square,part) || !prepared_storage::add_array(bytes,part,sizeof(MarkovTransition)+sizeof(std::size_t)) ||
       !prepared_storage::add_array(bytes,square,sizeof(float)))return false;
    out=bytes;maximum_edges=edges;return true;
}
std::size_t PreparedMetricalTransitions::workspace_bytes() const noexcept {
    std::size_t bytes=edges_.capacity()*sizeof(MarkovTransition)+boundaries_.capacity()*sizeof(std::vector<MarkovTransition>)+
        boundary_indices_.capacity()*sizeof(std::vector<std::size_t>);
    for(const auto& p:boundaries_)bytes+=p.capacity()*sizeof(MarkovTransition);
    for(const auto& p:boundary_indices_)bytes+=p.capacity()*sizeof(std::size_t);
    return bytes;
}
}
