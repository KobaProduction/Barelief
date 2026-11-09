#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
std::pair<unsigned,unsigned> lags(const Config& cfg,unsigned length) noexcept {
    const auto low=std::max(1U,unsigned(std::ceil(60*cfg.feature_rate_fps/cfg.max_bpm)));
    const auto high=std::min(length>8?length-8:0U,unsigned(std::floor(60*cfg.feature_rate_fps/cfg.min_bpm)));
    return {low,high};
}
bool center_values(Engine& e,unsigned length) noexcept {
    for(unsigned i=0;i<length;++i)e.centered[i]=e.flux[e.flux.count-length+i];
    auto values=std::span(e.centered).first(length);float mean=0;
    if(!arithmetic_mean_float32(values,mean))return false;
    for(auto& value:values)value-=mean;
    e.centered_count=length;return true;
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
namespace {
void append_acf_evidence(Engine& e,std::span<const float> scores,unsigned low) noexcept {
    for(std::size_t i=0;i<scores.size();++i){
        const auto lag=low+i;
        if(scores[i]>=0)e.acf.push({rounded(60.f*float(e.cfg.feature_rate_fps)/float(lag),3),scores[i],lag});
    }
}
}
bool Engine::calculate_acf(unsigned length) noexcept {
    centered_count=0;lag_count=0;acf.clear();
    if(!center_values(*this,length))return false;
    const auto [low,high]=lags(cfg,length);first_lag=low;
    if(high<low)return true;
    auto scores=std::span(lag_scores).first(high-low+1);
    constexpr OverlapEnergyPolicy policy{1e-18f,-1.f,0.f,1.f,true};
    if(!overlap_energy_autocorrelation(std::span(centered).first(length),low,scores,policy))return false;
    lag_count=scores.size();
    append_acf_evidence(*this,scores,low);
    return true;
}
void Engine::refine(const Frame& frame,bool ambiguous) noexcept {
    const auto cost=possible_ops();out.possible_cost_units=cost;
    out.schedule=schedule(*this,frame,ambiguous,cost);
    if(out.schedule!=Schedule::Full && out.schedule!=Schedule::Gated)return;
    if(!calculate_acf(std::min(cfg.acf_window,unsigned(flux.count))))return;
    last_heavy_seq=frame.sequence;last_heavy_obs=frame.t_obs;
    out.heavy_executed=true;out.cost_units=cost;out.schedule=mode==Mode::Full?Schedule::Full:Schedule::Gated;
}
}
