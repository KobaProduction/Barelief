#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
template<class T> bool rank_order(const T& a,const T& b) noexcept {return std::tuple{-a.score,a.bpm}<std::tuple{-b.score,b.bpm};}
Family family(const Candidate& c) noexcept {return {c.period_s,c.bpm,c.score,c.accent.score,c.accent.available};}
bool family_preference(const Candidate& a,const Candidate& b) noexcept {
    return std::tuple{a.accent.available,a.accent.score,a.score,-a.bpm}>std::tuple{b.accent.available,b.accent.score,b.score,-b.bpm};
}
void latest(std::optional<double>& value,double time) noexcept {value=std::max(value.value_or(time),time);}
bool better_phase(Phase phase,std::uint32_t group,const Candidate& candidate) noexcept {
    return std::tuple{phase.fit,phase.value,group}>=std::tuple{candidate.phase_support,candidate.phase,candidate.phase_group.value_or(0)};
}
void add_shadows(Engine& e) noexcept {
    const auto count=e.periods.count;
    for(std::size_t i=0;i<count;++i){const double period=e.periods.values[i];e.propose(period/2);e.propose(period*2);}
}
void support_group(Candidate& c,const Group& g,std::size_t slot) noexcept {
    const double io=ioi_support(g.times,c.period_s);
    c.ioi=std::max(c.ioi,io);c.group_support[slot]=g.id==0?std::max(io,c.acf):io;
    const auto phase=phase_fit(g.times,c.period_s);
    if(better_phase(phase,g.id,c)){c.phase=phase.value;c.phase_support=phase.fit;c.phase_group=g.id;c.phase_t_obs=g.times.back();}
    if(c.group_support[slot]>0)latest(c.t_obs,g.times.back());
}
void fallback_phase(const Engine& e,Candidate& c) noexcept {
    const auto phase=phase_fit(e.times,c.period_s);c.phase=phase.value;c.phase_support=phase.fit;
    if(e.times.count)c.phase_t_obs=e.times.back();
}
double event_support(const Engine& e,Candidate& c) noexcept {
    unsigned active=0;double support=0;
    for(std::size_t i=0;i<e.groups.count;++i){
        const auto& g=e.groups.values[i];
        if(g.times.count<3)continue;
        support_group(c,g,i);support+=c.group_support[i];++active;
    }
    if(active)return support/active;
    fallback_phase(e,c);
    return 0;
}
double temporal_penalty(const Engine& e,const Candidate& c,double now) noexcept {
    if(!e.last_determined || now-e.last_determined->second>e.cfg.max_age_s)return 0;
    return std::min(.1,.1*std::abs(60/c.period_s-e.last_determined->first)/std::max(1.,e.last_determined->first));
}
void group_proposals(Engine& e,const Group& g) noexcept {
    if(g.times.count<3)return;
    for(std::size_t i=1;i<g.times.count;++i){
        const double delta=g.times[i]-g.times[i-1];
        if(delta>0){e.propose(delta*.5);e.propose(delta);e.propose(delta*2);}
    }
}
void unique_periods(Engine& e,bool descending) noexcept {
    auto values=e.periods.span();
    if(descending)std::sort(values.begin(),values.end(),std::greater<>{});
    else std::sort(values.begin(),values.end());
    e.periods.count=std::size_t(std::unique(values.begin(),values.end())-values.begin());
}
struct FamilyCluster {
    Candidate best;
    double first=0,previous=0;
    void append(Engine& e,const Candidate& value) noexcept {
        const bool joined=first>0 && value.bpm-previous<=.015*value.bpm && value.bpm-first<=.03*value.bpm;
        if(joined){if(family_preference(value,best))best=value;}
        else {if(first>0)e.families.push(family(best));best=value;first=value.bpm;}
        previous=value.bpm;
    }
};
}
void Engine::propose(double period) noexcept {
    if(period>=60/cfg.max_bpm && period<=60/cfg.min_bpm)periods.push(rounded(period,6));
}
void Engine::proposals(bool use_acf) noexcept {
    periods.clear();
    for(const auto& g:groups.span())group_proposals(*this,g);
    if(use_acf)for(const auto& value:acf.span())if(value.score>0)propose(60/value.bpm);
    // Match Python's set before adding shadows, including rounded periods.
    unique_periods(*this,false);add_shadows(*this);unique_periods(*this,true);
}
void Engine::candidate_support(Candidate& c,double now,bool use_acf) noexcept {
    if(use_acf)for(const auto& value:acf.span())if(std::abs(value.bpm-60/c.period_s)<1.05)c.acf=std::max(c.acf,value.score);
    const double support=event_support(*this,c);
    if(c.acf>0 && last_heavy_obs)latest(c.t_obs,*last_heavy_obs);
    const double temporal=temporal_penalty(*this,c,now);
    c.phase_penalty=c.phase?.1*(1-c.phase_support):0;
    c.score=rounded(std::max(0.,support-temporal-c.phase_penalty),6);
}
Candidate Engine::candidate(double period,double now,bool use_acf) noexcept {
    Candidate value;value.period_s=period;value.bpm=rounded(60/period,4);
    candidate_support(value,now,use_acf);
    value.accent=hierarchy(period,now);candidate_phases(value);
    value.ioi=rounded(value.ioi,6);value.acf=rounded(value.acf,6);
    if(value.phase)value.phase=rounded(*value.phase,8);
    value.phase_support=rounded(value.phase_support,6);value.phase_penalty=rounded(value.phase_penalty,6);
    for(auto& score:value.group_support)score=rounded(score,6);
    return value;
}
void Engine::cluster(double now,bool use_acf) noexcept {
    families.clear();FamilyCluster cluster;
    for(double period:periods.span())cluster.append(*this,candidate(period,now,use_acf));
    if(cluster.first>0)families.push(family(cluster.best));
    auto values=families.span();std::sort(values.begin(),values.end(),rank_order<Family>);
}
void Engine::keep_candidate(const Candidate& value) noexcept {
    for(std::size_t i=0;i<out.candidate_count;++i)if(out.candidates[i].period_s==value.period_s)return;
    if(out.candidate_count<out.candidates.size())out.candidates[out.candidate_count++]=value;
}
namespace {
void keep_salient(Engine& e,double now,bool use_acf) noexcept {
    std::array<const Family*,2> best{};
    for(const auto& value:e.families.span()){
        if(!value.accented)continue;
        if(!best[0] || value.accent_score>best[0]->accent_score){best[1]=best[0];best[0]=&value;}
        else if(!best[1] || value.accent_score>best[1]->accent_score)best[1]=&value;
    }
    for(const auto* value:best)if(value)e.keep_candidate(e.candidate(value->period_s,now,use_acf));
}
void keep_octave(Engine& e,double aim,double now,bool use_acf) noexcept {
    if(aim<e.cfg.min_bpm || aim>e.cfg.max_bpm)return;
    const Family* closest=nullptr;
    for(const auto& value:e.families.span()){
        const double distance=std::abs(value.bpm-aim);
        if(distance>.015*aim)continue;
        if(!closest || std::tuple{distance,-value.score}<std::tuple{std::abs(closest->bpm-aim),-closest->score})closest=&value;
    }
    if(closest)e.keep_candidate(e.candidate(closest->period_s,now,use_acf));
}
}
void Engine::rank(double now,bool use_acf) noexcept {
    proposals(use_acf);cluster(now,use_acf);out.candidate_count=0;
    for(std::size_t i=0;i<std::min(std::size_t{12},families.count);++i)keep_candidate(candidate(families.values[i].period_s,now,use_acf));
    keep_salient(*this,now,use_acf);const auto count=out.candidate_count;
    for(std::size_t i=0;i<count;++i){const double bpm=out.candidates[i].bpm;keep_octave(*this,bpm*.5,now,use_acf);keep_octave(*this,bpm*2,now,use_acf);}
    std::sort(out.candidates.begin(),out.candidates.begin()+std::ptrdiff_t(out.candidate_count),rank_order<Candidate>);
}
}
