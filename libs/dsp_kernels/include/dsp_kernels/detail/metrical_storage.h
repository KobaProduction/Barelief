#pragma once
#include <dsp_kernels/metrical_transitions.h>
#include <dsp_kernels/prepared_storage.h>
#include <array>
#include <algorithm>

namespace dsp_kernels::detail {
struct MetricalStorage {
    std::size_t states=0,beats=0,periods=0,space=0,transitions=0,edges=0,model=0,total=0;
};
inline bool accumulate_metrical_geometry(const MetricalPatternConfig& c,MetricalStorage& s) noexcept {
    const std::array config{c};
    std::size_t states=0,edges=0,bytes=0,count=0,sum=0;
    if(!PreparedMetricalStateSpace::geometry(config,states,edges) ||
       !PreparedMetricalStateSpace::storage_bytes(config,bytes) ||
       !period_grid_bound(c.minimum_interval,c.maximum_interval,count,sum) ||
       !prepared_storage::add_bytes(s.states,states) ||
       !prepared_storage::add_bytes(s.space,bytes) ||
       !prepared_storage::add_bytes(s.beats,c.beats))return false;
    s.periods=std::max(s.periods,count);
    return true;
}
inline bool finish_metrical_storage(std::size_t patterns,std::size_t frames,std::size_t densities,bool switches,MetricalStorage& s) noexcept {
    if(s.states>UINT32_MAX ||
       !PreparedMetricalTransitions::storage_bytes(s.states,patterns,s.beats,s.periods,switches,s.transitions,s.edges) ||
       !PreparedSparseMarkovModel::storage_bytes(frames,densities,s.states,s.edges,s.model))return false;
    s.total=s.space;
    return prepared_storage::add_bytes(s.total,s.transitions) &&
        prepared_storage::add_bytes(s.total,s.model) &&
        prepared_storage::add_array(s.total,s.states,sizeof(std::uint32_t)) &&
        prepared_storage::add_array(s.total,s.beats,sizeof(TempoTransitionRule)) &&
        prepared_storage::add_array(s.total,patterns,sizeof(MetricalPatternConfig));
}
// Describes explicit model geometry without constructing any states, edges or
// FFT plans. Integer-grid bounds cover the source's unique logarithmic grids.
template<class Read>
bool metrical_storage(std::size_t patterns,Read read,std::size_t frames,std::size_t densities,bool switches,MetricalStorage& out) noexcept {
    if(!patterns)return false;
    MetricalStorage storage;
    for(std::size_t p=0;p<patterns;++p){
        MetricalPatternConfig config;
        if(!read(p,config) || !accumulate_metrical_geometry(config,storage))return false;
    }
    if(!finish_metrical_storage(patterns,frames,densities,switches,storage))return false;
    out=storage;
    return true;
}
}
