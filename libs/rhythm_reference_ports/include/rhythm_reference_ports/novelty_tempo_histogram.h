// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/fourier_tempo_histogram.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct NoveltyTempoHistogramConfig {
    float frame_rate=44100.f/512.f,frame_seconds=4,overlap=16;
    float minimum_bpm=30,maximum_bpm=560,minimum_change_seconds=5,induced_bpm=0;
    std::size_t zero_padding_factor=0,maximum_peaks=50;
    SpectralWindowShape window=SpectralWindowShape::Hann;
    bool weight_by_magnitude=true,constant_tempo=false;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
// Complete source novelty->BpmHistogram numerical recipe, without PCM frontend,
// I/O or executor. Reuses temporal framing, real/complex Fourier, polar math,
// per-frame candidates, ranking, continuity and phase reconstruction.
// Source power-of-two frame rounding, centered half-valid end rule, normalized
// symmetric analysis window, zero-phase [second-half,zeros,first-half] layout,
// unscaled FFT and max-normalized synthesis window are explicit here.
// Magnitude/phase rows are recomputed on demand with one frame workspace;
// no full input novelty or complex spectrum history is cloned.
// An explicit raw window overrides only the shape, with source normalization.
class PreparedNoveltyTempoHistogram {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t maximum,NoveltyTempoHistogramConfig,std::span<const float> window,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    static bool dimensions(std::size_t maximum,NoveltyTempoHistogramConfig,std::size_t& frame_size,std::size_t& fft_size,std::size_t& hop,std::size_t& frames) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_novelty_samples,NoveltyTempoHistogramConfig={},
                 std::span<const float> raw_window={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> novelty) noexcept;
    void reset(bool reset_reference=false) noexcept {histogram_.reset(reset_reference);}
    const PreparedFourierTempoHistogram& result() const noexcept {return histogram_;}
    std::size_t frame_size() const noexcept {return spectrum_.config().length;}
    std::size_t fft_size() const noexcept {return fft_size_;}
    std::size_t hop() const noexcept {return spectrum_.config().hop;}
    std::span<const float> analysis_window() const noexcept {return analysis_window_;}
    std::span<const float> synthesis_window() const noexcept {return synthesis_window_;}
private:
    struct BorrowedInput {PreparedNoveltyTempoHistogram* self;std::span<const float> values;};
    bool configure(std::size_t,NoveltyTempoHistogramConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    bool geometry(NoveltyTempoHistogramConfig,dsp_kernels::TemporalWindowConfig&,std::size_t&) noexcept;
    bool storage_geometry(std::size_t,NoveltyTempoHistogramConfig,std::span<const float>,dsp_kernels::TemporalWindowConfig&,std::size_t&,std::size_t&) noexcept;
    bool windows(std::size_t,NoveltyTempoHistogramConfig,std::span<const float>);
    bool prepare_histogram(std::size_t,NoveltyTempoHistogramConfig);
    static bool read_magnitude(const void*,std::size_t,std::span<float>) noexcept;
    static bool read_phase(const void*,std::size_t,std::span<float>) noexcept;
    std::size_t maximum_samples_=0,fft_size_=0;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    PreparedFourierTempoHistogram histogram_;
    std::vector<float> analysis_window_,synthesis_window_,complex_;
};
}
