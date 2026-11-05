// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct BandRhythmTransformConfig {
    std::size_t frame_size=256,hop=32;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
// Essentia RhythmTransform: experimental / poor accuracy in source tests.
// Input rows [time][band] are energies from any explicitly supplied band bank;
// Mel is not hidden here. Signed first differences (first value zero), source
// normalized symmetric BH62 / zero-phase / raw FFT, then sum magnitude^2
// across bands in their input order. Every start < input frames is emitted,
// including ALL overlapping zero-padded tails, not only the first EOF frame.
// Callbacks are synchronous and not retained. Writer errors may leave a prefix.
// Only one analysis-window derivative matrix is retained, never full history.
class PreparedBandRhythmTransform {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t bands,BandRhythmTransformConfig,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    dsp_kernels::FourierTempogramView spectral_frame() const noexcept {return spectrum_.view();}
    bool prepare(std::size_t bands,BandRhythmTransformConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    std::size_t frames(std::size_t input_frames) const noexcept;
    std::size_t bins() const noexcept {return spectrum_.bins();}
    bool process(dsp_kernels::FloatMatrixSource,std::size_t input_frames,
                 dsp_kernels::FloatMatrixDestination) noexcept;
private:
    bool prepare_spectrum(const dsp_kernels::FourierBackendFactory*,std::span<float> coefficients);
    void prepare_buffers();
    bool read_row(dsp_kernels::FloatMatrixSource,std::size_t,std::span<float>) noexcept;
    bool difference_row(dsp_kernels::FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool differences(dsp_kernels::FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool transform_bands() noexcept;
    std::size_t bands_=0;
    BandRhythmTransformConfig config_;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    std::vector<float> derivative_,current_,previous_,complex_,magnitude_,power_;
};
}
