#include "state.h"

namespace dsp_kernels::rhythm::detail {
namespace {
bool preliminary_ambiguity(const Engine& e) noexcept {
    const auto& output=e.out;
    if(!output.candidate_count || output.candidates[0].score<e.cfg.score_threshold)return true;
    if(output.candidate_count>1 && output.candidates[0].score-output.candidates[1].score<e.cfg.score_margin)return true;
    return e.octave_decision().status==Status::Ambiguous;
}
void accept_features(Engine& e,const Frame& frame) noexcept {
    if(!frame.saturated){
        if(e.width)e.flux.push(e.out.activity);
        e.history_count=std::min(e.history_count+1,512U);
        std::copy(frame.features.begin(),frame.features.end(),e.features.begin());e.width=unsigned(frame.features.size());
    }
    e.profile=frame.profile;e.sample_rate=frame.sample_rate_hz;e.seq=frame.sequence;
    e.sample_index=frame.sample_index;e.t_obs=frame.t_obs;e.initialized=true;
}
void publish_periodicity(Engine& e,bool use_acf) noexcept {
    if(use_acf)for(const auto& value:e.acf.span())e.out.periodicity=std::max(e.out.periodicity.value_or(0),value.score);
}
}
void Engine::process(const Frame& frame) noexcept {
    clear_output(out);const auto reset_reason=accept_source(frame);out.reset=reset_reason;
    observe(frame);track_changes();accept_features(*this,frame);
    bool use_acf=last_heavy_obs && frame.t_obs-*last_heavy_obs<=cfg.max_age_s;
    bool ambiguous=true;
    if(mode==Mode::Gated){rank(frame.t_obs,use_acf);ambiguous=preliminary_ambiguity(*this);}
    refine(frame,ambiguous);use_acf=use_acf || out.heavy_executed;
    rank(frame.t_obs,use_acf);
    publish_periodicity(*this,use_acf);
    decide(frame,use_acf);
}
}
namespace dsp_kernels::rhythm {
struct Analyzer::State:detail::Engine {};
Analyzer::Analyzer()=default;
Analyzer::~Analyzer()=default;
Analyzer::Analyzer(Analyzer&&) noexcept=default;
Analyzer& Analyzer::operator=(Analyzer&&) noexcept=default;
Analyzer::Analyzer(const Analyzer& other):state_(other.state_?std::make_unique<State>(*other.state_):nullptr){}
Analyzer& Analyzer::operator=(const Analyzer& other) {
    if(this!=&other)state_=other.state_?std::make_unique<State>(*other.state_):nullptr;
    return *this;
}
bool Analyzer::prepare(const Config& config,Mode mode) {
    if(!detail::valid_config(config,mode))return false;
    auto prepared=std::make_unique<State>();prepared->cfg=config;prepared->mode=mode;
    state_=std::move(prepared);return true;
}
void Analyzer::reset() noexcept {if(state_)state_->reset();}
bool Analyzer::copy_state_from(const Analyzer& other) noexcept {
    if(!state_ || !other.state_ || state_->cfg!=other.state_->cfg || state_->mode!=other.state_->mode)return false;
    *state_=*other.state_;return true;
}
bool Analyzer::process(const Frame& frame) noexcept {
    if(!state_ || !detail::valid_frame(frame))return false;
    state_->process(frame);return true;
}
const Output& Analyzer::output() const noexcept {
    static const Output empty;
    return state_?state_->out:empty;
}
std::size_t Analyzer::workspace_bytes() noexcept {return sizeof(State);}
}
