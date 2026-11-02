#pragma once
#include <dsp_kernels/onsets.h>

namespace dsp_kernels {
inline constexpr std::size_t max_pending_onsets=256;
struct ConsolidatedOnset {
    OnsetPeak peak;
    std::uint16_t channels=0;
};
// Same-source channels are correlated. Merge different channels in a window
// anchored at the earliest attack, retain maximum strength/mass and their mask.
// A repeated channel starts another group. Pending detector peaks bound the
// event-time watermark: no use of future samples or fixed/optimistic readiness.
// One channel adds no merge window. Unfinished groups remain pending at EOF.
// Fixed storage; invalid input is atomic. Overflow discards the pending batch
// with an explicit dropped count; callers must treat it as a discontinuity.
class PreparedOnsetFusion {
public:
    bool prepare(unsigned channels,double window_s=.030) noexcept;
    void reset() noexcept;
    bool process(std::span<const OnsetObservation>,double t_obs,double t_ready) noexcept;
    std::span<const ConsolidatedOnset> output() const noexcept {return std::span{output_}.first(output_count_);}
    unsigned dropped() const noexcept {return dropped_;}
    static std::size_t workspace_bytes() noexcept {return sizeof(PreparedOnsetFusion);}
private:
    bool valid(std::span<const OnsetObservation>,double,double) const noexcept;
    bool valid_channel(const OnsetObservation&,unsigned,double,double) const noexcept;
    void append(std::span<const OnsetObservation>) noexcept;
    std::size_t group(ConsolidatedOnset&) const noexcept;
    void drain(double watermark,double ready) noexcept;
    unsigned channels_=0,dropped_=0;
    double window_s_=0,watermark_=0;
    std::optional<double> last_time_;
    std::array<std::optional<double>,max_onset_channels> last_peaks_{};
    std::array<ConsolidatedOnset,max_pending_onsets> pending_{},output_{};
    std::size_t pending_count_=0,output_count_=0;
};
}
