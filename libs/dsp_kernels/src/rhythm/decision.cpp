#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
auto ranked(const Output& out) noexcept {return std::span{out.candidates}.first(out.candidate_count);}
bool accent_order(const Candidate* a,const Candidate* b) noexcept {
    return std::tuple{-a->accent.score,-a->score,a->bpm}<std::tuple{-b->accent.score,-b->score,b->bpm};
}
bool any_octave(std::span<const Candidate> values) noexcept {
    for(std::size_t i=0;i<values.size();++i)for(std::size_t j=i+1;j<values.size();++j)if(octave(values[i].bpm,values[j].bpm))return true;
    return false;
}
Octave ambiguous(Reason reason) noexcept {return {Status::Ambiguous,reason,{},{},0};}
struct Rivals {
    bool relevant=false,comparable=false,acf=false;
    double octave_score=0,other_score=0;
};
void compare_rival(Rivals& result,const Candidate& c,const Candidate& winner,unsigned fps) noexcept {
    if(&c==&winner)return;
    if(octave(c.bpm,winner.bpm)){
        result.relevant=true;
        if(c.accent.available){result.comparable=true;result.octave_score=std::max(result.octave_score,c.accent.score);}
    }else if(!result.acf || !equivalent(c.period_s,winner.period_s,fps))result.other_score=std::max(result.other_score,c.score);
}
Rivals rivals(std::span<const Candidate> values,const Candidate& winner,unsigned fps) noexcept {
    Rivals result;
    for(const auto& c:values)result.acf=result.acf || c.acf>0;
    for(const auto& c:values)compare_rival(result,c,winner,fps);
    return result;
}
bool relock_qualified(const Candidate& c,double period,const Config& cfg) noexcept {
    return std::abs(c.period_s-period)/period<=cfg.relock_primary_period_tolerance
        && c.accent.available && c.accent.coverage>=.9 && c.accent.score>=cfg.octave_accent_min && c.tactus_phase;
}
bool blocked_relock(const Output& out,const Candidate& chosen,const Config& cfg) noexcept {
    for(const auto& c:ranked(out))if(octave(c.bpm,chosen.bpm) && c.accent.available && chosen.accent.score-c.accent.score<cfg.octave_accent_margin)return true;
    return false;
}
bool pending_change(const Engine& e,double now) noexcept {
    return e.change.present && e.anchor && e.stable_period && now>=e.change.confirm_s
        && now-e.change.confirm_s<=e.cfg.change_hold_s && !e.primary_conflict && !e.subdivision_conflict;
}
const Group* find_group(const Engine& e,std::uint32_t id) noexcept {
    for(const auto& value:e.groups.span())if(value.id==id)return &value;
    return nullptr;
}
Octave arbitrate(const Candidate& winner,const Candidate& second,const Rivals& other,const Config& cfg) noexcept {
    const double gap=winner.accent.score-other.octave_score;
    const double other_gap=other.acf?winner.score-other.other_score:winner.accent.score-second.accent.score;
    const bool ok=winner.accent.score>=cfg.octave_accent_min && gap>=cfg.octave_accent_margin && other_gap>=(other.acf?cfg.score_margin:cfg.octave_accent_margin);
    return {ok?Status::Determined:Status::Ambiguous,ok?Reason::TypedPhase:Reason::AccentContrast,
        ok?std::optional{winner.bpm}:std::nullopt,rounded(std::min(gap,other_gap),6),winner.accent.groups};
}
Octave qualified_octave(std::span<const Candidate* const> scores,const Rivals& other,const Config& cfg) noexcept {
    if(scores.size()<2)return ambiguous(Reason::NoIndependentAccents);
    if(!other.comparable)return ambiguous(Reason::NoComparableOctave);
    return arbitrate(*scores[0],*scores[1],other,cfg);
}
List<const Candidate*,42> accent_candidates(const Output& out) noexcept {
    List<const Candidate*,42> result;
    for(const auto& c:ranked(out))if(c.accent.available)result.push(&c);
    auto values=result.span();std::sort(values.begin(),values.end(),accent_order);return result;
}
std::optional<std::size_t> closest_relock(const Engine& e,double empirical) noexcept {
    std::optional<std::size_t> chosen;
    for(std::size_t i=0;i<e.out.candidate_count;++i){
        const auto& c=e.out.candidates[i];
        if(!relock_qualified(c,empirical,e.cfg))continue;
        if(!chosen || std::tuple{std::abs(c.period_s-empirical),-c.score}<std::tuple{std::abs(e.out.candidates[*chosen].period_s-empirical),-e.out.candidates[*chosen].score})chosen=i;
    }
    return chosen;
}
}
Octave Engine::octave_decision() const noexcept {
    const auto values=ranked(out);
    if(!any_octave(values))return {};
    const auto scored=accent_candidates(out);const auto scores=scored.span();
    if(scores.empty())return ambiguous(Reason::NoIndependentAccents);
    const auto& winner=*scores[0];const auto other=rivals(values,winner,cfg.feature_rate_fps);
    if(!other.relevant && &winner==&values[0])return {Status::Unknown,Reason::UnrelatedOctave,{},{},0};
    return qualified_octave(scores,other,cfg);
}
std::optional<std::size_t> Engine::relock(double now) const noexcept {
    if(!pending_change(*this,now))return {};
    const auto* g=find_group(*this,*anchor);
    if(!g || !g->strong.count || now-g->strong.back()>cfg.max_age_s)return {};
    double empirical=0,relative=0;
    if(!recent_intervals(g->strong,empirical,relative))return {};
    const auto chosen=closest_relock(*this,empirical);
    if(chosen && blocked_relock(out,out.candidates[*chosen],cfg))return {};
    return chosen;
}
void Engine::decide(const Frame& frame,bool use_acf) noexcept {
    out.octave=octave_decision();
    if(const auto chosen=relock(frame.t_obs)){
        const auto& c=out.candidates[*chosen];
        out.octave={Status::Determined,Reason::Relocked,c.bpm,{},c.accent.groups};
    }
    if(out.octave.chosen_bpm){
        const auto target=*out.octave.chosen_bpm;
        auto values=std::span{out.candidates}.first(out.candidate_count);
        const auto chosen=std::find_if(values.begin(),values.end(),[&](const Candidate& c){return c.bpm==target;});
        if(chosen!=values.end())std::rotate(values.begin(),chosen,chosen+1);
    }
    publish_tempo(frame,use_acf);publish_grid(frame);publish_dynamics(frame);publish_observations(frame);
}
}
