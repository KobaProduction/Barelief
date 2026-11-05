// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/beat_segment_energy.h>
#include <array>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
inline constexpr std::array<float,6> default_beats_loudness_borders{20,150,400,3200,7000,22000};
struct BeatLoudnessSequenceConfig {
    float sample_rate=44100,search_seconds=.1f,energy_seconds=.05f;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
// Source BeatsLoudness numerical composition. Default borders match STANDARD
// BeatsLoudness (five bands); its STREAMING counterpart uses the six-band
// default_single_beat_borders, supplied explicitly to the same implementation.
// Slices are shifted right at t<0, extended by the source 1ms margin, rounded
// as start + rounded duration, and sorted by (start,end). The first incomplete
// slice stops production; no zero-padding replaces source Slicer's early return.
// Caller PCM/ticks remain borrowed. Only schedule metadata and result rows are
// stored. Source input beat index is exposed because slice sorting changes order.
class PreparedBeatLoudnessSequence {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t maximum,BeatLoudnessSequenceConfig,std::span<const float> borders,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_beats,BeatLoudnessSequenceConfig={},
                 std::span<const float> borders_hz=default_beats_loudness_borders,const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> pcm,std::span<const float> beat_seconds) noexcept;
    std::span<const float> energies() const noexcept;
    std::span<const float> band_ratios(std::size_t row) const noexcept;
    std::span<const std::size_t> source_beat_indices() const noexcept;
    std::size_t rows() const noexcept {return ready_?count_:0;}
private:
    struct Slice {std::size_t begin=0,end=0,source=0;};
    bool configure(std::size_t,BeatLoudnessSequenceConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    bool slice(float,std::size_t,Slice&) const noexcept;
    bool schedule(std::span<const float>) noexcept;
    bool analyze(std::span<const float>) noexcept;
    BeatLoudnessSequenceConfig config_;
    PreparedBeatSegmentEnergy beat_;
    bool ready_=false;
    std::size_t count_=0,scheduled_=0;
    std::vector<Slice> slices_;
    std::vector<float> energies_,ratios_;
    std::vector<std::size_t> indices_;
};
}
