// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct AdaptiveOnsetSelectionConfig {
    std::size_t delay=5;
    float frame_rate=44100.f/512.f,alpha=.1f,silence_threshold=.02f;
};
// Onsets' post-smoothing selection: silence gate -> median+alpha*mean threshold
// -> source run cleanup. The first buffer write is frame1 into slot0, followed
// by slot (frame+1)%delay; this original indexing is intentional. An isolated
// final-frame detection is retained, as in the source. Offline/future context.
class PreparedAdaptiveOnsetSelection {
public:
    bool prepare(std::size_t maximum_frames,AdaptiveOnsetSelectionConfig={});
    static bool storage_bytes(std::size_t maximum_frames,AdaptiveOnsetSelectionConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return (gated_.capacity()+threshold_.capacity()+buffer_.capacity()+sorted_.capacity())*sizeof(float)+decisions_.capacity()+result_.capacity();
    }
    void reset() noexcept {result_frames_=0;}
    bool process(std::span<const float> smoothed,float cumulative_weights,
                 std::span<std::uint8_t> onsets) noexcept;
    bool onset_times(std::span<float> seconds,std::size_t& count) const noexcept;
    std::span<const float> gated_curve() const noexcept {return std::span(gated_).first(result_frames_);}
    std::span<const float> thresholds() const noexcept {return std::span(threshold_).first(result_frames_);}
    std::span<const std::uint8_t> decisions() const noexcept {return std::span(decisions_).first(result_frames_);}
private:
    bool gate(std::span<const float>,float) noexcept;
    float threshold() noexcept;
    bool decide(std::size_t) noexcept;
    void clean(std::size_t) noexcept;
    std::size_t maximum_frames_=0,result_frames_=0;
    AdaptiveOnsetSelectionConfig config_;
    std::vector<float> gated_,threshold_,buffer_,sorted_;
    std::vector<std::uint8_t> decisions_,result_;
};
}
