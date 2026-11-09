#pragma once
#include <dsp_kernels/sparse_markov.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {
struct BeatStateDecoderConfig {
    float frame_rate=100,min_bpm=55,max_bpm=215,transition_lambda=100,observation_lambda=16;
    float transition_floor=2.220446049250313e-16f,activation_threshold=0;
    std::size_t num_intervals=0;
    bool correct=true,fixed_interval=false;
};
// Source port of madmom's classical DBN beat decoder and beat-state HMM.
// Input MUST be explicit beat activations in [0,1], not an uncalibrated onset
// envelope. No trained network/weights are included. Offline Viterbi and online
// normalized forward decoding preserve their different future-context rules.
class PreparedBeatStateDecoder {
public:
    static bool storage_bytes(std::size_t frames,BeatStateDecoderConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,BeatStateDecoderConfig={});
    void reset() noexcept;
    bool offline(std::span<const float> activations,std::span<std::uint8_t> beats) noexcept;
    bool online(std::span<const float> activations,std::span<std::uint8_t> beats,bool reset_state=false) noexcept;
    std::span<const std::uint32_t> state_path() const noexcept {return {path_.data(),result_frames_};}
    std::size_t state_path_first_frame() const noexcept {return path_start_;}
    std::span<const std::uint32_t> intervals() const noexcept {return intervals_;}
    float online_bpm() const noexcept {return online_tempo_;}
    MarkovResultStatus path_status() const noexcept {return path_status_;}
    // Offline calls preserve the ongoing forward distribution/event counters.
    // Raw HMM paths for interval histograms: bypass activation cropping,
    // beat correction and event suppression. Share forward state with online().
    bool viterbi_states(std::span<const float> activations) noexcept;
    bool forward_states(std::span<const float> activations,bool reset_state=true) noexcept;
    std::span<const std::uint32_t> state_intervals() const noexcept {return state_intervals_;}
private:
    void allocate_state_buffers(std::size_t);
    bool append_period(std::uint32_t);
    void reserve_states(std::size_t,std::size_t);
    void append_internal_edges(std::vector<MarkovTransition>&) const;
    void append_period_edges(std::vector<MarkovTransition>&) const;
    bool decode_forward_path(std::span<const float>) noexcept;
    void trace_path(std::size_t,std::uint32_t) noexcept;
    bool decode_online_path(std::span<const float>) noexcept;
    void publish_online(std::span<std::uint8_t>) noexcept;
    bool configure(std::size_t,BeatStateDecoderConfig);
    bool build_intervals();
    bool build_states();
    bool build_transitions();
    void initialize(bool logarithmic) noexcept;
    void reset_online() noexcept;
    void crop(std::span<const float>,std::size_t& start,std::size_t& end) const noexcept;
    bool offline_beats(std::span<const float>,std::span<std::uint8_t>,std::size_t start) noexcept;
    bool accept_online(std::size_t state,std::uint64_t frame) noexcept;
    bool valid_activations(std::span<const float>) const noexcept;
    bool valid_input(std::span<const float>,std::span<std::uint8_t>) const noexcept;
    bool viterbi_frame(float activation,std::size_t frame) noexcept;
    void load_observations(float activation,bool logarithmic) noexcept;
    bool viterbi(std::span<const float>) noexcept;
    std::size_t forward(float activation) noexcept;
    void correct_beats(std::span<const float>,std::span<std::uint8_t>) const noexcept;
    void state_minima(std::span<std::uint8_t>) const noexcept;
    float observation(float activation,std::size_t state,bool logarithmic) const noexcept;
    BeatStateDecoderConfig config_;
    std::size_t maximum_frames_=0,states_=0,result_frames_=0,path_start_=0;
    std::uint64_t counter_=0,last_beat_frame_=0;
    float online_tempo_=0,last_beat_seconds_=0;
    MarkovResultStatus path_status_=MarkovResultStatus::Unprocessed;
    std::vector<std::uint32_t> intervals_,first_,last_,links_,path_,state_intervals_;
    std::vector<std::uint8_t> beat_state_,mask_;
    PreparedSparseMarkovTransitions transitions_;
    std::vector<float> positions_,transition_,densities_,previous_,current_,forward_previous_;
};
}
