#pragma once
#include <dsp_kernels/activation_peaks.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct ActivationPeakSequenceConfig {
    float frame_rate=100,threshold=.5f,smooth=0;
    float pre_average=0,post_average=0,pre_maximum=0,post_maximum=0;
    float combine=.03f,delay=0;
    bool online=false;
};
// Source OnsetPeakPickingProcessor: seconds-based settings, shared numerical
// peak kernel and left event combination. Online disables smoothing/future
// context. Its buffer length is determined by the FIRST nonempty chunk, and
// its counter advances by the complete chunk size, as in the source.
// process() defaults to resetting online state; reset_state=false continues it.
// Unsupported online delay is rejected at preparation (source raises on call).
// Empty input yields no events, avoiding the source BufferProcessor empty-write
// failure. No processing allocation; events remain valid until the next call.
class PreparedActivationPeakSequence {
public:
    bool prepare(std::size_t maximum_frames,ActivationPeakSequenceConfig={});
    static bool storage_bytes(std::size_t maximum_frames,ActivationPeakSequenceConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void reset() noexcept;
    bool process(std::span<const float> activations,bool reset_state=true) noexcept;
    bool process_offline(std::span<const float> activations) noexcept;
    bool process_online(std::span<const float> activations,bool reset_state=true) noexcept;
    std::span<const float> events() const noexcept {return std::span(events_).first(event_count_);}
    std::int64_t frame_counter() const noexcept {return counter_;}
    std::size_t buffer_frames() const noexcept {return started_?history_.values().size():0;}
private:
    void prepend_previous() noexcept;
    void publish_combined(std::size_t,std::size_t,bool) noexcept;
    bool configure(std::size_t,ActivationPeakSequenceConfig);
    bool prepare_kernels();
    bool validate(std::span<const float>) const noexcept;
    bool update_buffer(std::span<const float>,bool) noexcept;
    bool collect(dsp_kernels::PreparedActivationPeaks&,std::span<const float>,std::int64_t,float) noexcept;
    bool combine_online() noexcept;
    bool combine(bool with_previous,bool remember=false) noexcept;
    ActivationPeakSequenceConfig config_;
    dsp_kernels::ActivationPeakConfig peak_config_;
    dsp_kernels::PreparedActivationPeaks offline_,online_;
    ScalarHistory history_;
    std::vector<std::uint8_t> peaks_;
    std::vector<float> raw_,events_,smoothing_;
    std::size_t maximum_frames_=0,event_count_=0,raw_count_=0;
    std::int64_t counter_=0;
    float last_onset_=0;
    bool started_=false,has_last_=false;
};
}
