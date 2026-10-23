#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
double spread(const Ring<double,23>& values,double mean) noexcept {
    double largest=0;
    for(std::size_t i=0;i<values.count;++i)largest=std::max(largest,std::abs(values[i]-mean));
    return largest;
}
double average(const Ring<double,23>& values) noexcept {
    double sum=0;
    for(std::size_t i=0;i<values.count;++i)sum+=values[i];
    return sum/double(values.count);
}
bool in_range(const Engine& e,double period) noexcept {return period>=60/e.cfg.max_bpm && period<=60/e.cfg.min_bpm;}
bool strong_observation(const Engine& e,const Event& event) noexcept {
    return !event.evidence_omitted && strong(event.observation.kind)
        && event.observation.strength>=e.cfg.change_min_strong_strength;
}
bool update_baseline(Engine& e,Group& g,double candidate) noexcept {
    if(!g.stable_period){
        g.stable_period=candidate;
        if(!e.stable_period){e.stable_period=candidate;e.anchor=g.id;}
        return true;
    }
    if(std::abs(candidate-*g.stable_period)/ *g.stable_period>e.cfg.change_separation_relative)return false;
    g.stable_period=.9* *g.stable_period+.1*candidate;
    if(g.id==e.anchor)e.stable_period=g.stable_period;
    return true;
}
bool changed_incumbent(Engine& e,Group& g,double candidate) noexcept {
    if(update_baseline(e,g,candidate))return false;
    if(g.id!=e.anchor || !e.stable_period)return false;
    return std::abs(candidate-*e.stable_period)/ *e.stable_period>e.cfg.change_separation_relative;
}
void primary_warning(Engine& e,const Group& g,double now) noexcept {
    if(g.id!=e.anchor || !e.stable_period || g.strong.count<2)return;
    const double newest=now-g.strong[g.strong.count-2];
    if(std::abs(newest-*e.stable_period)/ *e.stable_period>e.cfg.change_separation_relative)e.primary_conflict=now;
    else e.primary_conflict.reset();
}
Group* accept_strong(Engine& e,const Event& event) noexcept {
    if(!strong_observation(e,event))return nullptr;
    auto* g=e.group(event.observation.group);
    if(!g)return nullptr;
    const double now=event.observation.t_obs;
    if(g->strong.count && now<=g->strong.back()+1e-6)return nullptr;
    g->strong.push(now);primary_warning(e,*g,now);return g;
}
bool subdivision_observation(const Engine& e,const Event& event) noexcept {
    const auto& a=event.observation;
    if(a.kind!=Kind::Hat || a.group==e.anchor || event.evidence_omitted || a.strength<e.cfg.subdivision_min_strength)return false;
    for(const auto& g:e.groups.span())if(g.id==a.group)return g.times.count>=4;
    return false;
}
void phase_innovation(Engine& e,double observed,double delta,double period) noexcept {
    if(std::abs(delta-period/2)>e.cfg.phase_innovation_tolerance_fraction*period){e.subdivision_conflict=observed;e.recovery=0;}
    else if(e.subdivision_conflict && ++e.recovery>=e.cfg.change_min_intervals){e.subdivision_conflict.reset();e.recovery=0;}
}
bool establish_spectral(Engine& e,double delta) noexcept {
    if(e.spectral_stable)return false;
    e.spectral_initial.push(delta);
    while(e.spectral_initial.count>e.cfg.change_min_intervals)e.spectral_initial.pop();
    if(e.spectral_initial.count<e.cfg.change_min_intervals)return true;
    const double mean=average(e.spectral_initial);
    if(spread(e.spectral_initial,mean)<=e.cfg.change_stability_relative*mean)e.spectral_stable=mean;
    else e.spectral_initial.pop();
    return true;
}
bool follow_spectral(Engine& e,double delta) noexcept {
    const double baseline=*e.spectral_stable;
    if(std::abs(delta-baseline)/baseline>e.cfg.change_separation_relative)return false;
    e.spectral_pending.clear();e.spectral_stable=.9*baseline+.1*delta;return true;
}
void confirm_spectral(Engine& e,double delta,double now) noexcept {
    if(e.spectral_pending.count){
        const double candidate=average(e.spectral_pending);
        if(std::abs(delta-candidate)>e.cfg.change_stability_relative*candidate)e.spectral_pending.clear();
    }
    e.spectral_pending.push(delta);
    while(e.spectral_pending.count>e.cfg.change_min_intervals)e.spectral_pending.pop();
    if(e.spectral_pending.count<e.cfg.change_min_intervals)return;
    const double period=average(e.spectral_pending),relative=spread(e.spectral_pending,period)/period;
    if(relative>e.cfg.change_stability_relative || std::abs(period-*e.spectral_stable)/ *e.spectral_stable<=e.cfg.change_separation_relative)return;
    e.spectral_change={true,true,rounded(60/ *e.spectral_stable,4),rounded(60/period,4),0,rounded(now,8),rounded(relative,6),0,e.cfg.change_min_intervals};
    e.spectral_stable=period;e.spectral_pending.clear();
}
}
bool Engine::recent_intervals(const Ring<double,24>& values,double& period,double& relative) const noexcept {
    if(values.count<cfg.change_min_intervals+1)return false;
    const auto first=values.count-cfg.change_min_intervals-1;
    period=(values.back()-values[first])/cfg.change_min_intervals;
    double largest=0;
    for(std::size_t i=first+1;i<values.count;++i)largest=std::max(largest,std::abs(values[i]-values[i-1]-period));
    relative=largest/period;
    return in_range(*this,period) && relative<=cfg.change_stability_relative;
}
void Engine::trim_episode(double start) noexcept {
    for(auto& g:groups.span())while(g.times.count && g.times[0]<start-1e-6)g.times.pop();
    while(times.count && times[0]<start-1e-6)times.pop();
    while(typed.count && typed[0].t_obs<start-1e-6)typed.pop();
    acf.clear();last_heavy_obs.reset();last_heavy_seq.reset();last_determined.reset();
}
void Engine::confirm_change(Group& g,double period,double start,double now,double relative) noexcept {
    change={true,false,rounded(60/ *stable_period,4),rounded(60/period,4),rounded(start,8),rounded(now,8),rounded(relative,6),g.id,cfg.change_min_intervals};
    g.stable_period=period;stable_period=period;primary_conflict.reset();subdivision_conflict.reset();
    recovery=0;tracking_start=start;trim_episode(start);
}
void Engine::strong_change(const Event& event) noexcept {
    auto* g=accept_strong(*this,event);
    if(!g)return;
    const double now=event.observation.t_obs;
    double candidate=0,relative=0;
    if(!recent_intervals(g->strong,candidate,relative))return;
    if(!changed_incumbent(*this,*g,candidate))return;
    confirm_change(*g,candidate,g->strong[g->strong.count-cfg.change_min_intervals-1],now,relative);
}
void Engine::spectral_rate(const Event& event) noexcept {
    if(event.observation.group!=0 || event.evidence_omitted)return;
    const double now=event.observation.t_obs;
    const auto previous=spectral_last;spectral_last=now;
    if(!previous)return;
    const double delta=now-*previous;
    if(!in_range(*this,delta)){spectral_pending.clear();spectral_initial.clear();return;}
    if(establish_spectral(*this,delta) || follow_spectral(*this,delta))return;
    confirm_spectral(*this,delta,now);
}
void Engine::subdivision(const Event& event) noexcept {
    if(!anchor || !stable_period)return;
    const auto* primary=group(*anchor);
    if(!primary || !primary->strong.count)return;
    const auto& observation=event.observation;
    if(!subdivision_observation(*this,event))return;
    const double delta=observation.t_obs-primary->strong.back();
    if(delta<0 || delta>*stable_period)return;
    phase_innovation(*this,observation.t_obs,delta,*stable_period);
}
void Engine::track_changes() noexcept {
    const auto events=std::span{out.events}.first(out.event_count);
    for(const auto& event:events)strong_change(event);
    for(const auto& event:events)spectral_rate(event);
    for(const auto& event:events)subdivision(event);
}
}
