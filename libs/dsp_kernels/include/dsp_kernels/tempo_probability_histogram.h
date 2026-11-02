#pragma once
#include <cstddef>
#include <span>

namespace dsp_kernels {
struct TempoProbabilityRange {std::size_t first=0,count=0;};
// madmom TCNTempoHistogram postprocessing of EXPLICIT tempo-class probabilities.
// Class i means i BPM; tuple/output-head selection is an adapter responsibility.
// Preserve argmax/argmin fallback-to-zero: if no class exceeds maximum_bpm,
// source max_idx=0 yields an EMPTY slice (do not silently extend it to the end).
bool tempo_probability_range(std::span<const float> probabilities,float minimum_bpm,float maximum_bpm,
                             TempoProbabilityRange&) noexcept;
bool tempo_probability_coordinates(TempoProbabilityRange,std::span<float> bpm) noexcept;
}
