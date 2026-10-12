#pragma once
#include <cstdint>
namespace signal_contracts {
struct OscillatorParameters {
    std::uint64_t id=1;
    bool enabled=true;
    double frequency_hz=440, amplitude=.5, phase_radians=0;
    double attack_seconds=0, release_seconds=0;
    bool operator==(const OscillatorParameters&) const = default;
};
}
