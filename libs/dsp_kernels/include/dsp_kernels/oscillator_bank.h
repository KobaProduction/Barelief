#pragma once
#include <signal_contracts/oscillator.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace dsp_kernels {
using OscillatorParameters=signal_contracts::OscillatorParameters;
// Fixed upper bound: neither updates nor rendering allocate. Admission may
// advertise a lower bound on constrained devices.
class OscillatorBank {
public:
    static constexpr std::size_t maximum_oscillators=signal_contracts::max_oscillators;
    static bool valid(std::span<const OscillatorParameters> p,std::uint32_t rate,std::size_t limit=maximum_oscillators) noexcept {
        if(!rate || p.empty() || p.size()>limit || p.size()>maximum_oscillators)return false;
        for(std::size_t i=0;i<p.size();++i){const auto& v=p[i];
            if(!v.id || !std::isfinite(v.frequency_hz) || v.frequency_hz<=0 || v.frequency_hz>=double(rate)/2 ||
               !std::isfinite(v.amplitude) || !std::isfinite(v.phase_radians) || !std::isfinite(v.attack_seconds) ||
               !std::isfinite(v.release_seconds) || v.attack_seconds<0 || v.release_seconds<0)return false;
            for(std::size_t j=0;j<i;++j)if(p[j].id==v.id)return false;
        }return true;
    }
    bool prepare(std::span<const OscillatorParameters> p,std::uint32_t rate,std::size_t limit=maximum_oscillators) noexcept {
        if(!valid(p,rate,limit))return false;
        count_=0;position_=0;rate_=rate;return update(p);
    }
    bool update(std::span<const OscillatorParameters> p) noexcept {
        if(!valid(p,rate_))return false;
        std::array<State,maximum_oscillators> next{};
        for(std::size_t i=0;i<p.size();++i)next[i]=updated_state(p[i]);
        states_=next;count_=p.size();return true;
    }
    // Seek the shared sample timeline without rendering skipped samples.
    // Existing frequency edits preserve phase; newly added oscillators anchor
    // their configured phase to this same sample position.
    bool seek(std::int64_t position) noexcept {
        if(position<0 || !rate_)return false;
        const auto delta=position-position_;
        if(delta){for(std::size_t i=0;i<count_;++i)seek_state(states_[i],delta);position_=position;}
        return true;
    }
    bool render(std::span<float> output) noexcept {
        if(!rate_ || !count_ || output.size()>std::uint64_t(INT64_MAX-position_))return false;
        for(auto& sample:output){const double sum=render_sum();
            if(!std::isfinite(sum) || std::fabs(sum)>3.4028234663852886e38)return false;
            sample=static_cast<float>(sum);++position_;
        }return true;
    }
private:
    static constexpr double tau=6.283185307179586476925286766559;
    static double wrap(double v) noexcept {v=std::fmod(v,tau);return v<0?v+tau:v;}
    struct State {OscillatorParameters parameters;double phase=0,level=0;};
    State updated_state(const OscillatorParameters& parameters)const noexcept {
        State result;result.parameters=parameters;
        result.phase=wrap(parameters.phase_radians+tau*std::fmod(double(position_)*parameters.frequency_hz/rate_,1.0));
        result.level=parameters.enabled&&!parameters.attack_seconds?1:0;
        for(std::size_t i=0;i<count_;++i)if(states_[i].parameters.id==parameters.id){
            result.phase=states_[i].parameters.phase_radians==parameters.phase_radians?states_[i].phase:wrap(parameters.phase_radians);
            result.level=states_[i].level;break;
        }
        return result;
    }
    void seek_state(State& state,std::int64_t delta)noexcept {
        state.phase=wrap(state.phase+tau*std::fmod(double(delta)*state.parameters.frequency_hz/rate_,1.0));
        if(delta<=0)return;
        const auto duration=state.parameters.enabled?state.parameters.attack_seconds:state.parameters.release_seconds;
        const double step=duration?double(delta)/(duration*rate_):1;
        state.level=state.parameters.enabled?std::fmin(1,state.level+step):std::fmax(0,state.level-step);
    }
    double render_oscillator(State& state)noexcept {
        const auto& parameters=state.parameters;const double duration=parameters.enabled?parameters.attack_seconds:parameters.release_seconds;
        const double step=duration?1/(duration*rate_):1;
        state.level=parameters.enabled?std::fmin(1,state.level+step):std::fmax(0,state.level-step);
        const auto value=parameters.amplitude*state.level*std::sin(state.phase);
        state.phase=wrap(state.phase+tau*parameters.frequency_hz/rate_);return value;
    }
    double render_sum()noexcept {double sum=0;for(std::size_t i=0;i<count_;++i)sum+=render_oscillator(states_[i]);return sum;}
    std::array<State,maximum_oscillators> states_{};
    std::size_t count_=0;std::uint32_t rate_=0;std::int64_t position_=0;
};
}
