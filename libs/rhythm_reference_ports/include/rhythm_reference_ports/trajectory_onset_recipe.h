// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/triangular_spectral_bands.h>
#include <rhythm_reference_ports/buffered_band_flux.h>
#include <rhythm_reference_ports/contrast_peak_picker.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <rhythm_reference_ports/frame_stage_outputs.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct TrajectoryOnsetRecipeConfig {
    std::size_t frame_size=2048,audio_hop=256,novelty_chunk_size=4096;
    float sample_rate=44100,band_sample_rate=44100;
    float threshold=.05f,ratio_threshold=16,combine_ms=20;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
struct TrajectoryOnsetFrameStages {
    SpectralFrameStages spectrum;
    std::span<const float> bands,delayed_bands;
    std::span<const double> band_centers_hz;
    float band_sample_rate=0;
    // The first two audio frames have no novelty token. Token0 is audio frame2.
    bool novelty_available=false;
    std::size_t novelty_index=0;
    float novelty=0;
};
// Source SuperFluxExtractor numerical PCM recipe. No I/O/executor/UI.
// Centered ceil(N/2) frames through the first center >= EOF, source Hann
// normalization/zero-phase raw FFT, source fixed edges/power triangles,
// bin-width8/frame-width2 novelty, and source 4096-token peak accumulator.
// band_sample_rate is explicit: the source does not forward its sampleRate
// parameter into TriangularBands, whose default remains 44100.
// Source novelty/filter state survives network reset; clear_flux opts into a
// fresh filter. A final partial chunk changes the next acquisition size in
// the original (its reset omits AccumulatorAlgorithm::reset); that size is
// retained too, with restore_chunk_size available explicitly.
// All storage is prepared; only three derived band rows and one novelty chunk
// are buffered. Caller PCM is borrowed synchronously and never cloned.
// Stage outputs follow each audio frame, with no phase calculation requested
// by this magnitude-only source path. Reset/failure invalidates the last view.
// Source: https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/superfluxextractor.cpp#L42-L100
class PreparedTrajectoryOnsetRecipe {
public:
    bool prepare(std::size_t maximum_pcm_samples,std::size_t maximum_onsets,TrajectoryOnsetRecipeConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(std::size_t maximum_pcm_samples,std::size_t maximum_onsets,TrajectoryOnsetRecipeConfig,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool process(std::span<const float> pcm,FrameStageOutputPort<TrajectoryOnsetFrameStages> output={}) noexcept;
    TrajectoryOnsetFrameStages frame_stages() const noexcept;
    void reset(bool clear_flux=false,bool restore_chunk_size=false) noexcept;
    std::span<const float> onsets() const noexcept {return std::span(onsets_).first(onset_count_);}
    std::size_t acquisition_size() const noexcept {return acquisition_size_;}
private:
    bool configure(std::size_t,std::size_t,TrajectoryOnsetRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_spectrum(TrajectoryOnsetRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_features(TrajectoryOnsetRecipeConfig);
    bool prepare_peaks(std::size_t,TrajectoryOnsetRecipeConfig);
    bool frame(std::span<const float>,std::size_t) noexcept;
    bool novelty_frame() noexcept;
    bool delay_bands() noexcept;
    bool flush(bool final) noexcept;
    std::size_t maximum_pcm_=0,onset_count_=0,band_frames_=0,pending_=0,acquisition_size_=0,configured_chunk_=0;
    std::size_t frame_index_=0;
    bool frame_ready_=false,novelty_available_=false;
    float frame_novelty_=0;
    TrajectoryOnsetRecipeConfig config_;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    PreparedTriangularSpectralBands bands_;
    PreparedBufferedBandFlux novelty_;
    PreparedContrastPeakAccumulator peaks_;
    std::vector<ScalarHistory> history_;
    std::vector<float> complex_,magnitude_,band_values_,delayed_,chunk_,onsets_;
};
}
