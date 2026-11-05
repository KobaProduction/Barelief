// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
namespace rhythm_reference_ports {
// Explicit uniform [0,1] source. Seed/state belongs to its supplying adapter.
struct UniformNoisePort {
    void* context=nullptr;
    float (*next)(void*) noexcept=nullptr;
};
}
