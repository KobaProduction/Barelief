#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace dsp_kernels {
// Source int(seconds*fps): nonnegative Float32 duration, truncating toward zero.
inline bool truncated_frame_count(float seconds,float frame_rate,std::size_t& output) noexcept {
    if(!std::isfinite(seconds) || seconds<0 || !std::isfinite(frame_rate) || frame_rate<=0)return false;
    const float frames=seconds*frame_rate;
    if(!std::isfinite(frames) || frames>=float(INT32_MAX/4))return false;
    output=std::size_t(frames);return true;
}
// Float32 seconds -> integer frames, nearest with ties to even, independent
// of the process floating-point rounding mode. Zero is a valid duration.
// The bound keeps subsequent signed frame/window arithmetic representable.
inline bool rounded_frame_count(float seconds,float frame_rate,std::size_t& output) noexcept {
    if(!std::isfinite(seconds) || seconds<0 || !std::isfinite(frame_rate) || frame_rate<=0)return false;
    const float frames=seconds*frame_rate;
    if(!std::isfinite(frames) || frames>=float(INT32_MAX/4))return false;
    const float lower=std::floor(frames),fraction=frames-lower;
    const auto integer=std::size_t(lower);
    output=integer+std::size_t(fraction>.5f || (fraction==.5f && integer%2));
    return true;
}
}
