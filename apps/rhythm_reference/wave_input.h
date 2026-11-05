// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <QString>
#include <cstdint>

struct WaveInputInfo {
    std::uint64_t frames=0;
    std::uint32_t sample_rate=0;
};
// Cold RIFF/fmt/data validation only. Sample decoding remains AudioDecoder's job.
// Rejects compressed/extensible/RF64/RIFX containers explicitly.
WaveInputInfo inspect_wave_input(const QString&);
