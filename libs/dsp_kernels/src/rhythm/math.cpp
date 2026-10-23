#include "state.h"

namespace dsp_kernels::rhythm::detail {
void clear_output(Output& output) noexcept {
    std::destroy_at(&output);std::construct_at(&output);
}
double rounded(double value,unsigned digits) noexcept {
    const double factor=std::pow(10.,double(digits));
    // Round half to even independently of the process floating-point mode.
    const double scaled=value*factor,lo=std::floor(scaled),fraction=scaled-lo;
    const double up=fraction>.5 || (fraction==.5 && std::fmod(lo,2.)!=0);
    return (lo+up)/factor;
}
double modulo(double value,double period) noexcept {
    const double result=std::fmod(value,period);
    return result<0?result+period:result;
}
bool strong(Kind kind) noexcept {return kind==Kind::Bass || kind==Kind::Snare || kind==Kind::Clap;}
bool octave(double a,double b) noexcept {
    const auto [low,high]=std::minmax(a,b);
    return low>0 && std::abs(high/(2*low)-1)<=.015;
}
bool equivalent(double a,double b,unsigned fps) noexcept {
    if(a<=0 || b<=0 || fps==0)return false;
    const double difference=std::abs(a-b);
    return difference<=.5/fps && difference/std::min(a,b)<=.02;
}
namespace {
template<class Times> Phase fit(const Times& times,std::size_t count,double period) noexcept {
    if(count<2 || period<=0)return {};
    double real=0,imaginary=0;
    for(std::size_t i=0;i<count;++i){
        const double delta=times[i]-times[0];
        const double angle=std::abs(delta/period)>1e12?2*std::numbers::pi*(modulo(delta,period)/period):2*std::numbers::pi*delta/period;
        real+=std::cos(angle);imaginary+=std::sin(angle);
    }
    const double coherence=std::hypot(real,imaginary)/double(count);
    if(coherence<1e-12)return {};
    return {modulo(times[0]+std::atan2(imaginary,real)*period/(2*std::numbers::pi),period),std::min(1.,coherence)};
}
bool unit(double value) noexcept {return std::isfinite(value) && value>=0 && value<=1;}
bool positive(double value) noexcept {return std::isfinite(value) && value>0;}
bool valid_thresholds(const Config& c) noexcept {
    return unit(c.score_threshold) && unit(c.score_margin) && unit(c.phase_threshold)
        && positive(c.max_age_s) && positive(c.max_event_age_s)
        && positive(c.onset_threshold) && c.onset_threshold<1 && c.onset_min_positive_u8>0
        && positive(c.onset_refractory_s) && positive(c.octave_accent_min) && c.octave_accent_min<=1
        && positive(c.octave_accent_margin) && c.octave_accent_margin<=1;
}
bool valid_accents(const Config& c) noexcept {
    return positive(c.accent_phase_tolerance) && c.accent_phase_tolerance<.25
        && unit(c.accent_min_strength) && unit(c.subdivision_min_strength) && positive(c.accent_horizon_s)
        && c.change_min_intervals>=2 && c.change_min_intervals<=23
        && positive(c.change_stability_relative) && c.change_stability_relative<c.change_separation_relative
        && c.change_separation_relative<.5 && positive(c.change_hold_s) && unit(c.change_min_strong_strength)
        && positive(c.phase_innovation_tolerance_fraction) && c.phase_innovation_tolerance_fraction<.25
        && positive(c.relock_primary_period_tolerance) && c.relock_primary_period_tolerance<.1;
}
bool valid_geometry(const Config& c) noexcept {
    return (c.feature_rate_fps==30 || c.feature_rate_fps==45 || c.feature_rate_fps==60)
        && c.acf_min_frames>=12 && c.acf_window>=c.acf_min_frames && c.acf_window<=512
        && positive(c.min_bpm) && positive(c.max_bpm) && c.min_bpm<c.max_bpm
        && c.max_bpm/c.min_bpm<=10 && c.min_bpm>=1 && c.max_bpm<=3600
        && c.refinement_gap_frames>=1 && c.min_events>=2 && c.min_events<=96
        && c.max_provenance_groups>=1 && c.max_provenance_groups<=8
        && positive(c.min_ioi_s) && positive(c.max_ioi_s) && c.min_ioi_s<c.max_ioi_s;
}
bool valid_accent(const Accent& event,double now) noexcept {
    return std::isfinite(event.t_obs) && event.t_obs>=0 && event.t_obs<=now
        && unit(event.strength) && unsigned(event.kind)<=unsigned(Kind::Unknown);
}
}
Phase phase_fit(std::span<const double> times,double period) noexcept {return fit(times,times.size(),period);}
Phase phase_fit(const Ring<double,96>& times,double period) noexcept {return fit(times,times.count,period);}
double ioi_support(const Ring<double,96>& times,double period) noexcept {
    if(times.count<3 || period<=0)return 0;
    double total=0;
    for(std::size_t i=1;i<times.count;++i){
        const double delta=times[i]-times[i-1],nearest=std::max(1.,rounded(delta/period,0));
        total+=std::max(0.,1-std::abs(delta-nearest*period)/(.18*period));
    }
    return total/double(times.count-1);
}
Estimate estimate(Status status,std::optional<double> score,std::optional<double> observed,
                  const Frame& frame,Reason reason) noexcept {
    Estimate result;result.status=status;result.score=score;result.t_obs=observed;
    result.t_pub=frame.t_pub;result.reason=reason;
    if(observed){result.t_ready=frame.t_ready;result.age_s=std::max(0.,frame.t_pub-*observed);}
    return result;
}
bool valid_config(const Config& c,Mode mode) noexcept {
    return unsigned(mode)<=unsigned(Mode::Gated) && valid_geometry(c) && valid_thresholds(c) && valid_accents(c);
}
bool valid_frame(const Frame& f) noexcept {
    if(!positive(f.sample_rate_hz) || !std::isfinite(f.t_obs) || !std::isfinite(f.t_ready) || !std::isfinite(f.t_pub))return false;
    if(f.t_obs<0 || f.t_obs>f.t_ready || f.t_ready>f.t_pub)return false;
    if(f.features.size()!=64 && f.features.size()!=96)return false;
    if(!f.accents)return true;
    if(f.accents->size()>256)return false;
    return std::all_of(f.accents->begin(),f.accents->end(),[&](const Accent& a){return valid_accent(a,f.t_obs);});
}
}
