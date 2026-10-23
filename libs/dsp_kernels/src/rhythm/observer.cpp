#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
bool same_event(const Accent& a,const Accent& b) noexcept {
    return rounded(a.t_obs,6)==rounded(b.t_obs,6) && a.provenance==b.provenance && a.kind==b.kind;
}
unsigned kind_order(Kind kind) noexcept {
    constexpr std::array<unsigned,5> order{0,3,1,2,4};
    return order[unsigned(kind)];
}
bool event_order(const Event& a,const Event& b) noexcept {
    const auto& x=a.observation;const auto& y=b.observation;
    return std::tuple{x.t_obs,x.provenance,kind_order(x.kind)}<std::tuple{y.t_obs,y.provenance,kind_order(y.kind)};
}
template<class T,std::size_t N,class Time> void expire_ring(Ring<T,N>& ring,double oldest,Time time) noexcept {
    while(ring.count && time(ring[0])<oldest)ring.pop();
}
void reset_tracking(Engine& e) noexcept {
    e.last_onset.reset();e.last_heavy_obs.reset();e.last_heavy_seq.reset();e.last_determined.reset();
    e.stable_period.reset();e.anchor.reset();e.tracking_start.reset();e.primary_conflict.reset();e.subdivision_conflict.reset();
    e.spectral_last.reset();e.spectral_stable.reset();e.spectral_initial.clear();e.spectral_pending.clear();
    e.change={};e.spectral_change={};
}
Reset source_issue(const Engine& e,const Frame& f) noexcept {
    if(f.saturated)return Reset::Saturated;
    if(!e.initialized)return Reset::None;
    if(f.epoch!=e.epoch)return Reset::Epoch;
    if(f.profile!=e.profile || f.sample_rate_hz!=e.sample_rate || f.features.size()!=e.width)return Reset::Profile;
    if(e.seq==UINT64_MAX || f.sequence!=e.seq+1 || f.sample_index<=e.sample_index || f.t_obs<=e.t_obs)return Reset::Gap;
    if(std::abs(double(f.sample_index-e.sample_index)/f.sample_rate_hz-(f.t_obs-e.t_obs))>2/f.sample_rate_hz)return Reset::Gap;
    return Reset::None;
}
void ingest_evidence(Engine& e,const Accent& item,Group* group) noexcept {
    if(!group){++e.overflow;return;}
    auto& values=group->times;
    if(!values.count || item.t_obs-values.back()>1e-6)values.push(item.t_obs);
    e.typed.push(item);
}
void feature_flux(Engine& e,const Frame& f) noexcept {
    double positive=0,current=0,previous=0;
    for(std::size_t i=0;i<f.features.size();++i){
        e.contributions[i]=e.width?std::max(0,int(f.features[i])-int(e.features[i]))/255.:0;
        positive+=e.contributions[i];current+=f.features[i];previous+=e.features[i];
    }
    e.out.activity=positive/double(f.features.size());
    e.out.onset_contrast=e.width?positive*255/std::max({1.,current,previous}):0;
}
void explicit_events(Engine& e,const Frame& f) noexcept {
    for(const auto& event:*f.accents)e.out.events[e.out.event_count++].observation=event;
    std::sort(e.out.events.begin(),e.out.events.begin()+std::ptrdiff_t(e.out.event_count),event_order);
    const auto count=e.out.event_count;e.out.event_count=0;
    for(std::size_t i=0;i<count;++i)e.ingest(f,e.out.events[i].observation);
}
}
void Engine::reset(bool preserve_sequence) noexcept {
    if(!preserve_sequence)event_sequence=0;
    initialized=false;width=0;history_count=0;overflow=0;recovery=0;
    flux.clear();times.clear();typed.clear();seen.clear();groups.clear();acf.clear();
    reset_tracking(*this);clear_output(out);
}
Reset Engine::accept_source(const Frame& f) noexcept {
    const auto reason=source_issue(*this,f);
    if(reason!=Reset::None)reset(initialized && f.epoch==epoch);
    epoch=f.epoch;return reason;
}
Group* Engine::group(std::uint32_t id,bool create) noexcept {
    for(auto& value:groups.span())if(value.id==id)return &value;
    if(!create || groups.count>=cfg.max_provenance_groups)return nullptr;
    auto& value=groups.values[groups.count++];value={};value.id=id;return &value;
}
void Engine::ingest(const Frame&,Accent item) noexcept {
    for(std::size_t i=0;i<seen.count;++i)if(same_event(item,seen[i]))return;
    seen.push(item);
    auto* provenance_group=group(item.group,true);
    Event event{item,event_sequence++,provenance_group==nullptr};
    out.events[out.event_count++]=event;
    ingest_evidence(*this,item,provenance_group);
    if(!times.count || item.t_obs-times.back()>1e-6){times.push(item.t_obs);last_onset=item.t_obs;}
}
void Engine::expire(double now) noexcept {
    const auto time=[](double t){return t;};
    expire_ring(times,now-cfg.max_event_age_s,time);
    for(auto& g:groups.span())expire_ring(g.times,now-cfg.max_event_age_s,time);
    expire_ring(typed,now-cfg.accent_horizon_s,[](const Accent& a){return a.t_obs;});
}
namespace {
std::optional<Accent> observed_accent(const Engine& e,const Frame& f) noexcept {
    const double positive=e.out.activity*double(f.features.size())*255;
    if(!e.width || positive<e.cfg.onset_min_positive_u8 || e.out.onset_contrast<e.cfg.onset_threshold)return {};
    if(e.last_onset && f.t_obs-*e.last_onset<e.cfg.onset_refractory_s)return {};
    std::array<double,4> partitions{};
    for(std::size_t i=0;i<f.features.size();++i)partitions[i*4/f.features.size()]+=e.contributions[i];
    const auto index=std::size_t(std::max_element(partitions.begin(),partitions.end())-partitions.begin());
    return Accent{f.t_obs,std::min(1.,e.out.onset_contrast),Kind(index),0,0};
}
}
void Engine::observe(const Frame& f) noexcept {
    feature_flux(*this,f);
    if(f.saturated)return;
    if(f.accents)explicit_events(*this,f);
    else if(const auto event=observed_accent(*this,f))ingest(f,*event);
    expire(f.t_obs);
}
}
