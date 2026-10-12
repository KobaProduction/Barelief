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
    static constexpr std::size_t maximum_oscillators=128;
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
        for(std::size_t i=0;i<p.size();++i){auto& s=next[i];s.parameters=p[i];s.phase=wrap(p[i].phase_radians+tau*std::fmod(double(position_)*p[i].frequency_hz/rate_,1.0));
            s.level=p[i].enabled && !p[i].attack_seconds?1:0;
            for(std::size_t j=0;j<count_;++j)if(states_[j].parameters.id==p[i].id){
                s.phase=states_[j].parameters.phase_radians==p[i].phase_radians?states_[j].phase:wrap(p[i].phase_radians);
                s.level=states_[j].level;break;}
        }states_=next;count_=p.size();return true;
    }
    // Seek the shared sample timeline without rendering skipped samples.
    // Existing frequency edits preserve phase; newly added oscillators anchor
    // their configured phase to this same sample position.
    bool seek(std::int64_t position) noexcept {
        if(position<0 || !rate_)return false;
        const auto delta=position-position_;
        if(delta){for(std::size_t i=0;i<count_;++i){auto& s=states_[i];
            s.phase=wrap(s.phase+tau*std::fmod(double(delta)*s.parameters.frequency_hz/rate_,1.0));
            if(delta>0){const auto duration=s.parameters.enabled?s.parameters.attack_seconds:s.parameters.release_seconds;
                const double step=duration?double(delta)/(duration*rate_):1;
                s.level=s.parameters.enabled?std::fmin(1,s.level+step):std::fmax(0,s.level-step);}}
            position_=position;}
        return true;
    }
    bool render(std::span<float> output) noexcept {
        if(!rate_ || !count_ || output.size()>std::uint64_t(INT64_MAX-position_))return false;
        for(auto& sample:output){double sum=0;
            for(std::size_t i=0;i<count_;++i){auto& s=states_[i];const auto& p=s.parameters;
                const double duration=p.enabled?p.attack_seconds:p.release_seconds;
                const double step=duration?1/(duration*rate_):1;
                s.level=p.enabled?std::fmin(1,s.level+step):std::fmax(0,s.level-step);
                sum+=p.amplitude*s.level*std::sin(s.phase);
                s.phase=wrap(s.phase+tau*p.frequency_hz/rate_);
            }
            if(!std::isfinite(sum) || std::fabs(sum)>3.4028234663852886e38)return false;
            sample=static_cast<float>(sum);++position_;
        }return true;
    }
private:
    static constexpr double tau=6.283185307179586476925286766559;
    static double wrap(double v) noexcept {v=std::fmod(v,tau);return v<0?v+tau:v;}
    struct State {OscillatorParameters parameters;double phase=0,level=0;};
    std::array<State,maximum_oscillators> states_{};
    std::size_t count_=0;std::uint32_t rate_=0;std::int64_t position_=0;
};
}
