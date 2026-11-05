// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/pcm_predictive_onset.h>
#include <rhythm_reference_ports/predictive_beat_tracker.h>
#include <dsp_kernels/prepared_resampler.h>
#include <memory>
namespace rhythm_reference_ports {
struct PcmPredictiveBeatObservation {
    float onset=0,bpm=120;
    BeatPredictionObservation prediction;
};
// BTrack whole PCM recipe at source Fs=44100: ODF -> predictor -> independent
// whole-window sinc -> comb/tempo transition. Factory is supplied explicitly;
// desktop table/provider is not linked from this portable GPL target.
// Complete fixed hops only; framing and PCM resampling remain external stages.
class PreparedPcmPredictiveBeatTracker {
public:
    PreparedPcmPredictiveBeatTracker()=default;
    PreparedPcmPredictiveBeatTracker(const PreparedPcmPredictiveBeatTracker&)=delete;
    PreparedPcmPredictiveBeatTracker& operator=(const PreparedPcmPredictiveBeatTracker&)=delete;
    PreparedPcmPredictiveBeatTracker(PreparedPcmPredictiveBeatTracker&&) noexcept=default;
    PreparedPcmPredictiveBeatTracker& operator=(PreparedPcmPredictiveBeatTracker&&) noexcept=default;
    bool prepare(const dsp_kernels::ResamplerBackendFactory&,PcmPredictiveOnsetConfig={},
                 const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(const dsp_kernels::ResamplerBackendFactory&,PcmPredictiveOnsetConfig,std::size_t&,
                              const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    std::unique_ptr<PreparedPcmPredictiveBeatTracker> clone() const;
    bool process_hop(std::span<const float>,PcmPredictiveBeatObservation&) noexcept;
    bool process_onset(float,PcmPredictiveBeatObservation&) noexcept;
    bool update_hop_and_frame(std::uint32_t hop,std::uint32_t frame,const dsp_kernels::FourierBackendFactory* =nullptr);
    bool restart() noexcept;
    bool set_method(PredictiveOnsetMethod method) noexcept {return frontend_.set_method(method);}
    bool set_tempo(float bpm) noexcept {return tracker_.set_tempo(bpm);}
    bool fix_tempo(float bpm) noexcept {return tracker_.fix_tempo(bpm);}
    void release_tempo() noexcept {tracker_.release_tempo();}
    float bpm() const noexcept {return tracker_.bpm();}
    const PreparedPcmPredictiveOnset& frontend() const noexcept {return frontend_;}
    std::span<const float> onset_window() const noexcept {return tracker_.onset_window();}
    std::span<const float> cumulative_scores() const noexcept {return tracker_.cumulative_scores();}
    dsp_kernels::ResamplerResources resampler_resources() const noexcept;
    const PreparedPredictiveBeatTracker& tracker_stage() const noexcept {return tracker_;}
private:
    static bool resample(void*,std::span<const float>,std::span<float>) noexcept;
    static std::unique_ptr<dsp_kernels::PreparedResampler> prepare_resampler(
        const dsp_kernels::ResamplerBackendFactory&,std::uint32_t hop,dsp_kernels::NumericalPrecision);
    OnsetWindowResamplingPort resampling_port(dsp_kernels::PreparedResampler&) const noexcept;
    PreparedPcmPredictiveOnset frontend_;
    PreparedPredictiveBeatTracker tracker_;
    // Stable pointee keeps the borrowed callback context valid across moves.
    std::unique_ptr<dsp_kernels::PreparedResampler> resampler_;
    const dsp_kernels::ResamplerBackendFactory* factory_=nullptr;
};
}
