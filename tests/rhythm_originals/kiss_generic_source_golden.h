#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace kiss_generic_source {
// Genuine original Factory FFT, N2206: first .75, last -.25, otherwise0.
// Provenance: Testing/xhigh-kiss-generic-20261028/source-oracles/real2206/manifest.json.
inline constexpr std::array<std::size_t,9> bins{0,1,2,7,49,97,551,1102,1103};
inline constexpr std::array<std::array<std::uint32_t,2>,9> values{{
    {0x3f000000u,0x00000000u},
    {0x3f000011u,0xba3aab27u},
    {0x3f000044u,0xbabaa885u},
    {0x3f000341u,0xbba35141u},
    {0x3f009f4eu,0xbd0e72f0u},
    {0x3f026d53u,0xbd8ba95au},
    {0x3f3fe8abu,0xbe7fffefu},
    {0x3f7fffefu,0xba3aab27u},
    {0x3f800000u,0x00000000u},
}};
}
