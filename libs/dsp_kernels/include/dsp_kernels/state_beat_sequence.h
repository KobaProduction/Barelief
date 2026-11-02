#pragma once
#include <dsp_kernels/beat_state_decoder.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace dsp_kernels {
// DBNBeatTrackingProcessor event/time wrapper over the shared beat-state HMM.
// Supplied beat probabilities only; preserves source offline/online distinction,
// initial first-beat suppression, reset default=true, counter and BPM state.
// Calling offline does not reset the continuing online stream.
class PreparedStateBeatSequence {
public:
    static bool storage_bytes(std::size_t frames,BeatStateDecoderConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return decoder_.workspace_bytes()+flags_.capacity()+events_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_frames,BeatStateDecoderConfig={},bool online=false);
    void reset() noexcept;
    bool process(std::span<const float>,bool reset_state=true) noexcept;
    bool process_offline(std::span<const float>) noexcept;
    bool process_online(std::span<const float>,bool reset_state=true) noexcept;
    std::span<const float> events() const noexcept {return std::span(events_).first(count_);}
    std::uint64_t frame_counter() const noexcept {return counter_;}
    float online_bpm() const noexcept {return decoder_.online_bpm();}
    const PreparedBeatStateDecoder& decoder() const noexcept {return decoder_;}
private:
    bool publish(std::size_t frames,std::uint64_t first_frame) noexcept;
    PreparedBeatStateDecoder decoder_;
    std::vector<std::uint8_t> flags_;
    std::vector<float> events_;
    std::size_t maximum_frames_=0,count_=0;
    std::uint64_t counter_=0;
    float frame_rate_=0;
    bool online_=false;
};
}
