#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
namespace dsp_kernels {
enum class EventCoordinateUnit {Frames,Samples,Seconds};
// Exact counters and time coordinates, separate from Float32 DSP arithmetic.
inline bool event_sample_position(std::size_t frame,std::size_t hop,std::uint64_t& output) noexcept {
    if(!hop || std::uint64_t(frame)>UINT64_MAX/hop)return false;
    output=std::uint64_t(frame)*hop;return true;
}
inline bool event_time_position(std::size_t frame,std::size_t hop,float rate,double& output) noexcept {
    std::uint64_t sample=0;
    if(!std::isfinite(rate) || rate<=0 || !event_sample_position(frame,hop,sample))return false;
    output=double(sample)/double(rate);return true;
}
}
