#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
double margin(const Output& out,bool use_acf,unsigned fps) noexcept {
    const auto& top=out.candidates[0];double best=-1;
    for(std::size_t i=1;i<out.candidate_count;++i){
        const auto& c=out.candidates[i];
        if(use_acf && equivalent(top.period_s,c.period_s,fps))continue;
        best=std::max(best,c.score);
    }
    return best<0?1:top.score-best;
}
Reason suspend(const Engine& e,const Frame& frame) noexcept {
    if(e.primary_conflict)return Reason::PrimaryConflict;
    if(e.subdivision_conflict)return Reason::SubdivisionConflict;
    if(!e.stable_period || !e.anchor)return Reason::None;
    for(const auto& g:e.groups.span()){
        if(g.id!=e.anchor || !g.strong.count || frame.t_obs-g.strong.back()>e.cfg.max_age_s)continue;
        if(std::abs(e.out.candidates[0].period_s-*e.stable_period)/ *e.stable_period>e.cfg.change_separation_relative)return Reason::PrimaryConflict;
    }
    return Reason::None;
}
bool held(const Change& change,const Frame& frame,const Config& cfg) noexcept {
    return change.present && frame.t_pub>=change.confirm_s && frame.t_pub-change.confirm_s<=cfg.change_hold_s;
}
double next_tick(double now,double phase,double period) noexcept {
    double tick=phase+std::max(0.,std::ceil((now-phase)/period-1e-10))*period;
    if(tick<now-1e-8)tick+=period;
    return rounded(tick,8);
}
void publish_grid_value(Engine& e,const Frame& frame,const Candidate& c) noexcept {
    const bool anchored=e.out.tempo.status==Status::Determined && e.out.octave.status==Status::Determined && c.tactus_phase;
    const bool fresh=c.tactus_t_obs && frame.t_pub-*c.tactus_t_obs<=e.cfg.max_age_s;
    const auto observed=c.tactus_t_obs?c.tactus_t_obs:c.t_obs;
    const auto fallback=e.out.tempo.status==Status::Unknown?Status::Unknown:Status::Ambiguous;
    e.out.rhythm_grid=estimate(anchored && fresh?Status::Determined:fallback,c.tactus_support,observed,frame,Reason::NoAnchor);
    if(!observed){e.out.rhythm_grid=estimate(Status::Unknown,{},{},frame,Reason::NoAnchor);return;}
    e.out.grid_period_s=c.period_s;
    if(!anchored || !fresh)return;
    e.out.grid_phase_s=c.tactus_phase;e.out.next_beat_s=next_tick(frame.t_pub,*c.tactus_phase,c.period_s);
    e.out.rhythm_grid.reason=Reason::TypedPhase;
}
std::pair<Status,Reason> tempo_status(const Engine& e,const Frame& frame,bool use_acf) noexcept {
    const auto& top=e.out.candidates[0];
    if(!top.t_obs || frame.t_pub-*top.t_obs>e.cfg.max_age_s)return {Status::Unknown,Reason::Stale};
    if(e.out.octave.status==Status::Ambiguous)return {Status::Ambiguous,Reason::Octave};
    const bool distinct=e.out.octave.status==Status::Determined || margin(e.out,use_acf,e.cfg.feature_rate_fps)>=e.cfg.score_margin;
    if(e.times.count<e.cfg.min_events || top.score<e.cfg.score_threshold || !distinct)return {Status::Ambiguous,Reason::Weak};
    const auto conflict=suspend(e,frame);
    return {conflict==Reason::None?Status::Determined:Status::Ambiguous,conflict};
}
void publish_diagnostics(Engine& e) noexcept {
    auto& out=e.out;
    out.history_frames=e.history_count;out.positive_changes=unsigned(e.flux.count);out.evidence_group_overflow=e.overflow;
    out.last_acf_t_obs=e.last_heavy_obs;out.tracking_start_s=e.tracking_start;out.confirmed_strong_ioi_s=e.stable_period;
    out.tempo_anchor_group=e.anchor;out.pending_primary_since=e.primary_conflict;out.pending_subdivision_since=e.subdivision_conflict;
    out.subdivision_recovery_count=e.recovery;
}
}
void Engine::publish_tempo(const Frame& frame,bool use_acf) noexcept {
    if(!out.candidate_count){out.tempo=estimate(Status::Unknown,{},{},frame,times.count?Reason::InsufficientEvidence:Reason::NoEvents);return;}
    const auto& top=out.candidates[0];
    const auto [status,reason]=tempo_status(*this,frame,use_acf);
    const bool capped=out.octave.status==Status::Ambiguous || reason==Reason::PrimaryConflict || reason==Reason::SubdivisionConflict;
    out.raw_support=top.score;
    out.tempo=estimate(status,capped?std::min(top.score,.49):top.score,top.t_obs,frame,reason);
    if(status==Status::Determined){out.bpm=top.bpm;out.period_s=top.period_s;}
    if(held(change,frame,cfg))out.previous_episode=change;
}
void Engine::publish_grid(const Frame& frame) noexcept {
    if(!out.candidate_count){out.rhythm_grid=estimate(Status::Unknown,{},{},frame,Reason::NoAnchor);return;}
    publish_grid_value(*this,frame,out.candidates[0]);
}
namespace {
void derived_dynamics(Engine& e,const Frame& frame) noexcept {
    if(e.out.tempo.status!=Status::Determined || !e.out.tempo.t_obs)return;
    const double observed=*e.out.tempo.t_obs;
    if(e.last_determined && observed>e.last_determined->second+1e-9 && e.out.tempo_dynamics.status!=Status::Determined){
        e.out.bpm_per_s=rounded((*e.out.bpm-e.last_determined->first)/(observed-e.last_determined->second),6);
        e.out.tempo_dynamics=estimate(Status::Ambiguous,e.out.candidates[0].score,observed,frame,Reason::Weak);
    }
    e.last_determined=std::pair{*e.out.bpm,observed};
}
}
void Engine::publish_dynamics(const Frame& frame) noexcept {
    out.tempo_dynamics=estimate(Status::Unknown,{},{},frame,Reason::InsufficientEvidence);
    if(held(change,frame,cfg)){
        out.change=change;out.bpm_per_s=rounded((change.to_bpm-change.from_bpm)/std::max(1e-9,change.confirm_s-change.point_s),6);
        out.tempo_dynamics=estimate(Status::Determined,std::max(0.,1-change.spread),change.confirm_s,frame);
    }else if(held(spectral_change,frame,cfg)){
        out.change=spectral_change;
        out.tempo_dynamics=estimate(Status::Ambiguous,std::min(.49,std::max(0.,1-spectral_change.spread)),spectral_change.confirm_s,frame,Reason::Octave);
    }
    derived_dynamics(*this,frame);
}
void Engine::publish_observations(const Frame& frame) noexcept {
    std::optional<double> strength,observed;Status status=out.event_count?Status::Determined:Status::Unknown;
    for(std::size_t i=0;i<out.event_count;++i){
        const auto& e=out.events[i].observation;
        strength=std::max(strength.value_or(0),e.strength);observed=std::max(observed.value_or(e.t_obs),e.t_obs);
        if(e.kind==Kind::Unknown)status=Status::Ambiguous;
    }
    out.accent_events=estimate(status,strength,observed,frame,out.event_count?Reason::None:Reason::NoEvents);
    out.evidence=estimate(Status::Determined,{},frame.t_obs,frame);out.diagnostics=out.evidence;
    publish_diagnostics(*this);
}
}
