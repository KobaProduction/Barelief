// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/period_beat_tracker.h>
#include <rhythm_reference_ports/beat_agreement.h>
#include <array>
namespace rhythm_reference_ports {
struct OnsetCurveView {std::span<const float> values;};
struct MultiFeaturePeriodConfig {
    // Native source order: complex, RMS, Mel flux, beat emphasis, information gain.
    std::array<float,5> frame_rates{44100.f/1024.f,44100.f/1024.f,44100.f/1024.f,44100.f/512.f,44100.f/512.f};
    std::array<unsigned,5> upsampling{2,2,2,1,1};
    float minimum_bpm=40,maximum_bpm=208,agreement_minimum_seconds=5;
    UniformNoisePort noise;
};
// BeatTrackerMultiFeature's numerical composition from five explicit ODF curves.
// Each independent Degara candidate retains its native frame clock/upsampling;
// agreement selects the complete original sequence. PCM, framing, spectra and
// feature extraction remain independent upstream operations, with no graph I/O.
class PreparedMultiFeatureBeatTracker {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(const std::array<std::size_t,5>& maximum_frames,MultiFeaturePeriodConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(const std::array<std::size_t,5>& maximum_input_frames,MultiFeaturePeriodConfig={});
    bool process(const std::array<OnsetCurveView,5>&) noexcept;
    std::span<const float> ticks() const noexcept;
    std::span<const float> candidate_times(std::size_t method) const noexcept;
    std::span<const float> method_scores() const noexcept {return ready_?agreement_.method_scores():std::span<const float>{};}
    float confidence() const noexcept {return ready_?selection_.confidence:0;}
    std::size_t selected_method() const noexcept {return ready_?selection_.selected:std::numeric_limits<std::size_t>::max();}
private:
    bool configure(const std::array<std::size_t,5>&,MultiFeaturePeriodConfig);
    bool candidate(std::size_t,std::span<const float>) noexcept;
    bool prepared_=false,ready_=false;
    std::array<PreparedPeriodBeatTracker,5> trackers_;
    std::array<std::vector<std::uint8_t>,5> masks_;
    std::array<std::vector<float>,5> times_;
    std::array<std::size_t,5> counts_{};
    PreparedBeatAgreement agreement_;
    BeatAgreementSelection selection_;
};
}
