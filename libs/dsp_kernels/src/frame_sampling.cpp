// FramedSignal/signal_frame coordinate semantics from madmom audio/signal.py,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
// Only coordinates use double, matching source frame positioning; PCM is Float32.
#include <dsp_kernels/frame_sampling.h>
#include <cmath>
#include <cstdint>
namespace dsp_kernels {
bool PreparedFrameSampling::prepare(std::size_t maximum,FrameSamplingConfig c) noexcept {
    PreparedFrameSampling next;if(!next.configure(maximum,c))return false;
    *this=next;return true;
}
bool PreparedFrameSampling::frame_count(std::size_t maximum,FrameSamplingConfig c,std::size_t& out) noexcept {
    PreparedFrameSampling value;if(!value.configure(maximum,c))return false;
    out=value.frames(maximum);return true;
}
bool PreparedFrameSampling::configure(std::size_t maximum,FrameSamplingConfig c) noexcept {
    constexpr auto bound=std::size_t(PTRDIFF_MAX)/4;
    if(!maximum || maximum>bound || !c.length || c.length>bound ||
        !std::isfinite(c.hop_samples) || c.hop_samples<=0 ||
        c.origin < -std::ptrdiff_t(bound) || c.origin>std::ptrdiff_t(bound) ||
        unsigned(c.tail)>unsigned(FrameSamplingTail::Complete) || c.frame_count>bound)return false;
    config_=c;maximum_samples_=maximum;
    const auto count=frames(maximum);std::ptrdiff_t first=0;
    if(count && !first_sample(maximum,count-1,first))return false;
    if(!c.fixed_frames && !count)return false;
    return true;
}
std::size_t PreparedFrameSampling::frames(std::size_t samples) const noexcept {
    if(!maximum_samples_ || samples>maximum_samples_)return 0;
    if(config_.fixed_frames)return config_.frame_count;
    const double ratio=double(samples)/config_.hop_samples;
    const double extent=double(samples)+double(config_.center?2*(config_.length/2):0)-double(config_.length);
    const double count=config_.tail==FrameSamplingTail::Complete?(extent<0?0:1+std::floor(extent/config_.hop_samples)):
        config_.tail==FrameSamplingTail::Normal?std::ceil(ratio):std::floor(ratio+1);
    if(!std::isfinite(count) || count<0 || count>=double(PTRDIFF_MAX/4))return 0;
    return std::size_t(count);
}
bool PreparedFrameSampling::first_sample(std::size_t samples,std::size_t frame,std::ptrdiff_t& output) const noexcept {
    if(frame>=frames(samples))return false;
    const double reference=double(frame)*config_.hop_samples;
    if(!std::isfinite(reference) || reference>=double(PTRDIFF_MAX/4))return false;
    output=std::ptrdiff_t(reference)-std::ptrdiff_t(config_.center?config_.length/2:0)-config_.origin;return true;
}
}
