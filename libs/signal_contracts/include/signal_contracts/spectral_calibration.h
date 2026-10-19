#pragma once
#include <cmath>
namespace signal_contracts {
inline constexpr float maximum_profile_gain=1e6f;
inline bool valid_profile_gain(float value) noexcept {
    return std::isfinite(value) && value>0 && value<=maximum_profile_gain;
}
}
