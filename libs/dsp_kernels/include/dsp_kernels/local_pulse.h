#pragma once
#include <dsp_kernels/fourier_transform.h>
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/overlap_add.h>
#include <dsp_kernels/temporal_window.h>

namespace dsp_kernels {
struct LocalPulseConfig {
    TemporalWindowConfig window;
    float frame_rate=44100.f/512.f,min_bpm=30,max_bpm=300;
    bool limit_min=true,limit_max=true;
};
// Port of librosa.beat.plp from an EXPLICIT Fourier tempogram. Reader rows are
// [frame][bin][Re,Im], raw, one-sided, with DC/Nyquist. The supplied window,
// frame count and input rate must describe that same tempogram. This preserves
// phase; no magnitude-only reconstruction and no hidden onset/tempo estimator.
// Centered windows are offline estimates; output is a normalized pulse curve,
// not confirmed beat events. Storage is prepared; process() never allocates.
class PreparedLocalPulse {
public:
    static bool storage_bytes(std::size_t,LocalPulseConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_samples,LocalPulseConfig={},std::span<const float> coefficients={});
    bool process(FloatMatrixSource complex,std::size_t frames,std::span<float> pulse,
                 std::span<const float> log_prior={},FloatMatrixSource log_prior_rows={}) noexcept;
private:
    bool load(FloatMatrixSource,std::size_t,std::span<const float>) noexcept;
    bool select(std::span<const float>) noexcept;
    bool reconstruct(std::size_t frame) noexcept;
    bool finish(std::span<float>) noexcept;
    float divisor() const noexcept;
    PreparedTemporalWindow window_;
    PreparedFourierTransform inverse_;
    PreparedOverlapAdd overlap_;
    LocalPulseConfig config_;
    std::size_t maximum_samples_=0,bins_=0;
    std::vector<float> spectrum_,score_,real_,imaginary_,transformed_,frame_,pulse_,prior_row_;
};
}
