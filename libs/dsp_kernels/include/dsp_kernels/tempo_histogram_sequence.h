#pragma once
#include <dsp_kernels/beat_state_decoder.h>
#include <dsp_kernels/interval_histogram.h>
#include <dsp_kernels/state_interval_histogram.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class TempoHistogramEstimator {Autocorrelation,Comb,StateModel};
struct TempoHistogramSequenceConfig {
    TempoHistogramEstimator method=TempoHistogramEstimator::Comb;
    float frame_rate=100,minimum_bpm=40,maximum_bpm=250,history_seconds=10,alpha=.79f;
    bool online=false;
    // Rate/BPM bounds are supplied above; remaining state-model parameters are
    // explicit and used only for StateModel. No activation threshold/correction:
    // this recipe counts raw HMM states, not decoded beat events.
    BeatStateDecoderConfig state_model;
    std::size_t channels=1; // Multi-channel applies only to Comb, never ACF/DBN.
};
// ACF/Comb/DBNTempoHistogramProcessor recipes. All history and HMM storage is
// prepared. Offline Viterbi and online forward have independent state;
// offline ACF/comb calls do not erase the ongoing online history.
// DBN offline preserves the source's state-grid coordinates and dense bincount
// separately. A logarithmic grid may therefore yield unequal axis/bin lengths;
// the raw result exposes that source inconsistency rather than inventing an axis.
class PreparedTempoHistogramSequence {
public:
    static bool storage_bytes(std::size_t,TempoHistogramSequenceConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,TempoHistogramSequenceConfig={},std::span<const float> alpha={});
    void reset() noexcept;
    bool process(std::span<const float>,bool reset_state=true) noexcept;
    bool process_offline(std::span<const float>) noexcept;
    bool process_online(std::span<const float>,bool reset_state=true) noexcept;
    std::span<const float> bins() const noexcept;
    std::span<const float> channel_bins(std::size_t) const noexcept;
    std::size_t channels() const noexcept {return config_.channels;}
    std::span<const float> coordinates() const noexcept;
    std::span<const float> offline_coordinates() const noexcept;
    std::span<const float> online_coordinates() const noexcept {return coordinates_;}
    std::size_t offline_bin_count() const noexcept;
    const PreparedBeatStateDecoder& decoder() const noexcept {return decoder_;}
private:
    bool configure(std::size_t,TempoHistogramSequenceConfig,std::span<const float> alpha);
    bool valid_input_size(std::size_t) const noexcept;
    bool prepare_state_model(std::size_t,const IntervalHistogramConfig&);
    bool publish_counts(const PreparedStateIntervalHistogram&) noexcept;
    TempoHistogramSequenceConfig config_;
    PreparedIntervalHistogram offline_,online_;
    PreparedBeatStateDecoder decoder_;
    PreparedStateIntervalHistogram state_offline_,state_online_;
    std::vector<float> coordinates_,state_coordinates_,state_bins_;
    std::size_t maximum_frames_=0,result_count_=0,state_bin_count_=0;
    bool ready_=false,last_online_=false;
};
}
