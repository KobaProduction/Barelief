// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <span>

namespace rhythm_reference_ports {
struct TempoHistogramPeak {float bpm=0,weight=0,spread=0;};
struct BeatIntervalHistogram {
    std::array<float,250> histogram{};
    std::array<TempoHistogramPeak,2> peaks{};
};
// Source BpmHistogramDescriptors. Input is inter-beat intervals in seconds,
// not BPM values. Drops intervals below 60/250, rounds BPM into bins [0,249],
// and computes two peaks using +/-1 weight and +/-4 suppression/spread.
// No allocation; caller output is assigned after successful validation.
bool beat_interval_histogram(std::span<const float> intervals,BeatIntervalHistogram&) noexcept;
}
