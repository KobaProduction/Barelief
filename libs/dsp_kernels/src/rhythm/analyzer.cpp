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
        std::copy_n(e.current_features.begin(),feature_count(frame),e.features.begin());
        e.width=unsigned(feature_count(frame));e.feature_format=frame.features.index();
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
namespace {
void record_clipping(ClippingStatistics& statistics,const Frame& frame) noexcept {
    const auto* values=std::get_if<1>(&frame.features);
    if(!values)return;
    std::uint64_t clipped=0;
    for(const float value:*values){
        statistics.maximum_before_clamp=std::max(statistics.maximum_before_clamp,value);
        if(value>1.f)++clipped;
    }
    if(!clipped)return;
    constexpr auto limit=std::numeric_limits<std::uint64_t>::max();
    if(statistics.clipped_frames<limit)++statistics.clipped_frames;
    statistics.clipped_values+=std::min(clipped,limit-statistics.clipped_values);
}

OnsetConfig onset_config(const Config& config) noexcept {
    OnsetConfig result;result.feature_count=config.feature_count;
    result.channel_count=config.onset_channel_count;result.channels=config.onset_channels;
    result.contrast_floor=config.onset_threshold;result.refractory_s=config.onset_refractory_s;
    result.min_positive_mass=float(config.onset_min_positive_u8)/255.f;
    result.noise_multiplier=config.onset_noise_multiplier;return result;
}
}
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
    if(!prepared->onsets.prepare(onset_config(config)))return false;
    if(!prepared->onset_fusion.prepare(std::max(1U,config.onset_channel_count),config.onset_merge_s))return false;
    state_=std::move(prepared);return true;
}
void Analyzer::reset() noexcept {if(state_)state_->reset();}
bool Analyzer::copy_state_from(const Analyzer& other) noexcept {
    if(!state_ || !other.state_ || state_->cfg!=other.state_->cfg || state_->mode!=other.state_->mode)return false;
    *state_=*other.state_;return true;
}
bool Analyzer::process(const Frame& frame) noexcept {
    if(!state_ || !detail::valid_frame(frame) || detail::feature_count(frame)!=state_->cfg.feature_count)return false;
    state_->process(frame);record_clipping(state_->clipping,frame);return true;
}
const Output& Analyzer::output() const noexcept {
    static const Output empty;
    return state_?state_->out:empty;
}
ClippingStatistics Analyzer::clipping_statistics() const noexcept {
    return state_?state_->clipping:ClippingStatistics{};
}

AnalysisView Analyzer::analysis() const noexcept {
    if(!state_ || !state_->initialized)return {};
    const auto& e=*state_;
    const auto first=std::min(e.flux.count,e.flux.values.size()-e.flux.start);
    const auto history=std::span(e.flux.values);
    const bool acf=e.last_heavy_obs.has_value();
    return {std::span(e.features).first(e.width),std::span(e.contributions).first(e.width),
        history.subspan(e.flux.start,first),history.first(e.flux.count-first),
        std::span(e.centered).first(acf?e.centered_count:0),std::span(e.lag_scores).first(acf?e.lag_count:0),
        acf?e.acf.span():std::span<const LagEvidence>{},acf?e.first_lag:0,e.last_heavy_obs,e.t_obs,e.cfg.feature_rate_fps};
}
std::size_t Analyzer::workspace_bytes() noexcept {return sizeof(State);}
}
