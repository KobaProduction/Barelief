// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Experimental source Meter ("not evaluated"), not confirmed meter inference.
// Rows are explicitly [band][beat]. Raw FFT ACF per band, plain band sum, then
// FIRST maximum at lag>=2 gives the source meter output. The source power-
// harmonic histogram is retained as diagnostics; it DOES NOT replace that max.
// Silence therefore returns source lag2. Sequences shorter than three beats,
// for which source argmax has an empty population, are rejected safely.
// All nextPowerTwo(2*N) FFT plans are prepared for the configured beat capacity.
class PreparedCorrelationMeter {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t beats,std::size_t bands,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_beats,std::size_t maximum_bands,const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(dsp_kernels::FloatMatrixSource,std::size_t bands,std::size_t beats,float& meter) noexcept;
    std::span<const float> summed_correlation() const noexcept;
    std::span<const float> harmonic_histogram() const noexcept;
    std::span<const std::size_t> band_peak_lags() const noexcept;
    std::span<const float> band_peak_values() const noexcept;
private:
    void allocate(std::size_t,std::size_t);
    bool accumulate(dsp_kernels::FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool harmonic_histogram(std::size_t) noexcept;
    std::size_t maximum_beats_=0,maximum_bands_=0,beats_=0,bands_=0;
    bool ready_=false;
    dsp_kernels::PreparedVariableAutocorrelation correlation_;
    std::vector<float> row_,acf_,sum_,histogram_,band_maxima_;
    std::vector<std::size_t> band_lags_;
};
}
