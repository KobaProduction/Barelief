// SPDX-License-Identifier: AGPL-3.0-or-later
// Essentia source ports; see ../AGPL-COPYING and implementation provenance.
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <rhythm_reference_ports/causal_average.h>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
// Input is one magnitude spectrum, with the explicit source bin interval.
// Zero-initialized eleven-frame history; output describes the frame five
// frames behind the newest input. The source does not flush a padded tail.
class PreparedInformationGain {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t bins,std::size_t first_bin,std::size_t stop_bin,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t bins,std::size_t first_bin,std::size_t stop_bin);
    void reset() noexcept;
    bool process(std::span<const float> magnitude,float& detection) noexcept;
private:
    float bin_change(std::size_t) const noexcept;
    std::size_t bins_=0,first_=0,count_=0,next_=0;
    std::vector<float> history_;
};
struct BandBeatEmphasisConfig {
    float frame_rate=44100.f/512.f;
    std::size_t bands=40;
};
// Offline post-processing of explicit [frame][ERB band] onset rows.
// The preceding complex-distance and gammatone projection are independent
// operations. No PCM framing, Fourier, or filter-bank settings are inferred.
// Retains the source's forward 17-sample smoothing and appended tail in ACF.
// Source reserve/copy undefined behavior is replaced with a sized sort buffer.
class PreparedBandBeatEmphasis {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_frames,BandBeatEmphasisConfig,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,BandBeatEmphasisConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,
                 std::span<float> output) noexcept;
    std::span<const float> band_weights() const noexcept {return weights_;}
private:
    bool configure(std::size_t,BandBeatEmphasisConfig,const dsp_kernels::FourierBackendFactory*);
    void allocate();
    void tempo_weights(float rayleigh_scale) noexcept;
    void set_geometry(std::size_t,BandBeatEmphasisConfig,std::size_t) noexcept;
    bool read(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    bool normalize(std::span<float>,std::size_t) noexcept;
    bool threshold(std::span<float>,std::size_t) noexcept;
    bool weight(std::size_t,std::size_t) noexcept;
    float comb_weight() const noexcept;
    float select_weights() noexcept;
    bool combine(std::size_t,std::span<float>) noexcept;
    BandBeatEmphasisConfig config_;
    PreparedCausalAverage average_;
    dsp_kernels::PreparedVariableAutocorrelation correlation_;
    std::size_t maximum_frames_=0,stride_=0,maximum_period_=0;
    std::vector<float> rows_,row_,acf_,rayleigh_,weights_,sorted_,result_;
};
}
