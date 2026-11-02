// First-order attack/release envelope (Zolzer, Digital Audio Signal Processing,
// chapter 7). Independent Float32 numerical operation; no signal I/O or history.
#include <dsp_kernels/envelope_follower.h>
#include <algorithm>
#include <cmath>
#include <functional>

namespace dsp_kernels {
namespace {
bool nonnegative(float value) noexcept {return std::isfinite(value) && value>=0;}
float coefficient(float rate,float seconds) noexcept {
    return seconds>0?std::exp(-1.f/(rate*seconds)):0.f;
}
bool partial_overlap(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || input.data()==output.data())return false;
    const std::less<const float*> before;
    return before(input.data(),output.data()+output.size()) && before(output.data(),input.data()+input.size());
}
}
bool PreparedEnvelopeFollower::prepare(EnvelopeFollowerConfig config) noexcept {
    if(!std::isfinite(config.sample_rate) || config.sample_rate<=0 ||
       !nonnegative(config.attack_seconds) || !nonnegative(config.release_seconds))return false;
    PreparedEnvelopeFollower next;next.attack_=coefficient(config.sample_rate,config.attack_seconds);
    next.release_=coefficient(config.sample_rate,config.release_seconds);next.rectify_=config.rectify;
    next.flush_subnormal_=config.flush_subnormal_state;next.ready_=true;*this=next;return true;
}
bool PreparedEnvelopeFollower::process(float input,float& output) noexcept {
    if(!ready_ || !std::isfinite(input))return false;
    const float value=rectify_?std::abs(input):input,gain=state_<value?attack_:release_;
    const float next=(1.f-gain)*value+gain*state_;
    if(!std::isfinite(next))return false;
    output=next;state_=flush_subnormal_ && std::fpclassify(next)==FP_SUBNORMAL?0:next;return true;
}
bool PreparedEnvelopeFollower::process(std::span<const float> input,std::span<float> output,bool reset_before) noexcept {
    if(!ready_ || input.size()!=output.size() || partial_overlap(input,output) ||
       !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    if(reset_before)reset();
    for(std::size_t i=0;i<input.size();++i)if(!process(input[i],output[i]))return false;
    return true;
}
}
