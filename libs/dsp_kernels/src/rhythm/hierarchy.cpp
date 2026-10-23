#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
void unique_times(List<double,256>& times) noexcept {
    auto values=times.span();std::sort(values.begin(),values.end());
    times.count=std::size_t(std::unique(values.begin(),values.end())-values.begin());
}
bool separated(std::span<const double> strong_times,std::span<const double> weak_times,double period) noexcept {
    for(double a:strong_times)for(double b:weak_times)if(std::abs(a-b)<.08*period)return false;
    return true;
}
std::size_t occupied_ticks(std::span<const double> times,double phase,double period,double tolerance,double first,double last,std::span<double> workspace) noexcept {
    std::size_t occupied=0;
    for(double time:times){
        const double index=rounded((time-phase)/period,0);
        if(index<first || index>last || std::abs(time-(phase+index*period))>tolerance)continue;
        const auto values=workspace.first(occupied);
        if(std::find(values.begin(),values.end(),index)==values.end())workspace[occupied++]=index;
    }
    return occupied;
}
double coverage(std::span<const double> times,double phase,double period,double tolerance,std::span<double> workspace) noexcept {
    const double first=std::ceil((times.front()-phase)/period-1e-8);
    const double last=std::floor((times.back()-phase)/period+1e-8);
    const double total=last-first+1;
    if(total<3)return -1;
    if(!std::isfinite(total))return 0;
    return double(occupied_ticks(times,phase,period,tolerance,first,last,workspace))/total;
}
double offbeats(std::span<const double> times,double phase,double period,double tolerance) noexcept {
    unsigned count=0;
    for(double t:times)if(std::abs(modulo(t-phase,period)-period/2)<=tolerance)++count;
    return double(count)/double(times.size());
}
std::uint8_t group_bit(const Engine& e,std::uint32_t id) noexcept {
    for(std::size_t i=0;i<e.groups.count;++i)if(e.groups.values[i].id==id)return std::uint8_t(1U<<i);
    return 0;
}
void set_phase_values(Candidate& value,std::span<const double> salient,std::span<const double> weak) noexcept {
    const auto sub=phase_fit(weak,value.period_s);
    if(sub.value){value.subdivision_phase=rounded(*sub.value,8);value.subdivision_support=rounded(sub.fit,6);}
    if(!weak.empty())value.subdivision_t_obs=weak.back();
    if(!value.accent.phase)return;
    value.tactus_phase=value.accent.phase;
    value.tactus_support=rounded(phase_fit(salient,value.period_s).fit,6);
    if(!salient.empty())value.tactus_t_obs=salient.back();
}
Hierarchy score_hierarchy(Engine& e,double period,std::uint8_t groups) noexcept {
    Hierarchy result;result.available=true;result.groups=groups;
    result.strong_count=unsigned(e.salient.count);result.weak_count=unsigned(e.weak.count);
    const auto phase=phase_fit(e.salient.span(),period);
    if(!phase.value)return result;
    // ACF and candidate ranking are sequential; reuse prepared ACF scratch.
    const double occupied=coverage(e.salient.span(),*phase.value,period,e.cfg.accent_phase_tolerance*period,e.centered);
    if(occupied<0)return {};
    const double weak_fit=offbeats(e.weak.span(),*phase.value,period,e.cfg.accent_phase_tolerance*period);
    result.phase=rounded(*phase.value,8);result.fit=rounded(phase.fit,6);
    result.coverage=rounded(occupied,6);result.weak_offbeat=rounded(weak_fit,6);
    result.score=rounded(std::clamp(.5*phase.fit+.3*occupied+.2*weak_fit,0.,1.),6);
    return result;
}
}
void Engine::hierarchy_times(double now,std::uint8_t& strong_groups,std::uint8_t& weak_groups) noexcept {
    salient.clear();weak.clear();strong_groups=0;weak_groups=0;
    for(std::size_t i=0;i<typed.count;++i){
        const auto& event=typed[i];
        if(now-event.t_obs<0 || now-event.t_obs>cfg.accent_horizon_s)continue;
        if(strong(event.kind) && event.strength>=cfg.accent_min_strength){salient.push(rounded(event.t_obs,6));strong_groups|=group_bit(*this,event.group);}
        if(event.kind==Kind::Hat && event.strength>=cfg.subdivision_min_strength){weak.push(rounded(event.t_obs,6));weak_groups|=group_bit(*this,event.group);}
    }
}
Hierarchy Engine::hierarchy(double period,double now) noexcept {
    std::uint8_t sg=0,wg=0;hierarchy_times(now,sg,wg);
    if(salient.count<3 || weak.count<2 || !(sg&~wg) || !(wg&~sg))return {};
    unique_times(salient);unique_times(weak);
    if(salient.count<3 || weak.count<2 || !separated(salient.span(),weak.span(),period))return {};
    if(salient.values[salient.count-1]-salient.values[0]<2*period)return {};
    return score_hierarchy(*this,period,std::uint8_t(sg|wg));
}
void Engine::candidate_phases(Candidate& value) noexcept {
    salient.clear();weak.clear();
    for(std::size_t i=0;i<typed.count;++i){
        const auto& event=typed[i];
        if(!(group_bit(*this,event.group)&value.accent.groups))continue;
        if(strong(event.kind) && event.strength>=cfg.accent_min_strength)salient.push(rounded(event.t_obs,6));
        if(event.kind==Kind::Hat)weak.push(rounded(event.t_obs,6));
    }
    unique_times(salient);unique_times(weak);set_phase_values(value,salient.span(),weak.span());
}
}
