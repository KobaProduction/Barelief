#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
std::pair<unsigned,unsigned> lags(const Config& cfg,unsigned length) noexcept {
    const auto low=std::max(1U,unsigned(std::ceil(60*cfg.feature_rate_fps/cfg.max_bpm)));
    const auto high=std::min(length>8?length-8:0U,unsigned(std::floor(60*cfg.feature_rate_fps/cfg.min_bpm)));
    return {low,high};
}
double correlation(std::span<const double> values,unsigned lag) noexcept {
    double dot=0,left=0,right=0;
    for(std::size_t i=lag;i<values.size();++i){
        const double a=values[i],b=values[i-lag];dot+=a*b;left+=a*a;right+=b*b;
    }
    if(left<=1e-18 || right<=1e-18)return -1;
    return std::clamp(dot/std::sqrt(left*right),0.,1.);
}
void center_values(Engine& e,unsigned length) noexcept {
    double mean=0;
    for(unsigned i=0;i<length;++i){e.centered[i]=e.flux[e.flux.count-length+i];mean+=e.centered[i];}
    mean/=length;
    for(unsigned i=0;i<length;++i)e.centered[i]-=mean;
}
Schedule schedule(const Engine& e,const Frame& frame,bool ambiguous,unsigned cost) noexcept {
    if(e.mode==Mode::Light)return Schedule::Light;
    if(!cost || frame.saturated)return Schedule::History;
    if(e.mode==Mode::Full)return Schedule::Full;
    if(e.last_heavy_seq && frame.sequence-*e.last_heavy_seq<e.cfg.refinement_gap_frames)return Schedule::Gap;
    if(!ambiguous)return Schedule::Unambiguous;
    if(cost>frame.budget_ops.value_or(e.cfg.default_budget_ops))return Schedule::Budget;
    return Schedule::Gated;
}
}
unsigned Engine::possible_ops() const noexcept {
    const auto length=std::min(cfg.acf_window,unsigned(flux.count));
    if(length<cfg.acf_min_frames)return 0;
    const auto [low,high]=lags(cfg,length);
    unsigned result=0;
    for(unsigned lag=low;lag<=high;++lag)result+=length-lag;
    return result;
}
void Engine::calculate_acf(unsigned length) noexcept {
    center_values(*this,length);
    const auto [low,high]=lags(cfg,length);acf.clear();
    for(unsigned lag=low;lag<=high;++lag){
        const double score=correlation(std::span{centered}.first(length),lag);
        if(score>=0)acf.push({rounded(60.*cfg.feature_rate_fps/lag,3),score});
    }
}
void Engine::refine(const Frame& frame,bool ambiguous) noexcept {
    const auto cost=possible_ops();out.possible_cost_units=cost;
    out.schedule=schedule(*this,frame,ambiguous,cost);
    if(out.schedule!=Schedule::Full && out.schedule!=Schedule::Gated)return;
    calculate_acf(std::min(cfg.acf_window,unsigned(flux.count)));
    last_heavy_seq=frame.sequence;last_heavy_obs=frame.t_obs;
    out.heavy_executed=true;out.cost_units=cost;out.schedule=mode==Mode::Full?Schedule::Full:Schedule::Gated;
}
}
