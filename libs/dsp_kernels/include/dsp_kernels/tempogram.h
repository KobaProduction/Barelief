#pragma once
#include <dsp_kernels/fourier_transform.h>
#include <dsp_kernels/temporal_window.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <dsp_kernels/curve_interpolation.h>

namespace dsp_kernels {
enum class VectorNormalization {None,Maximum,Minimum,L0,Lp};
bool normalize_vector(std::span<float>,VectorNormalization,float exponent=2) noexcept;
struct FourierTempogramView {
    std::span<const float> windowed_input,transform_input,complex_values;
    std::size_t fft_size=0;
};

// Port of librosa 1.0.0 fourier_tempogram. One selected temporal frame produces
// N/2+1 raw complex bins [bin][Re,Im], including DC/Nyquist. Iterating frames()
// yields the complete offline matrix, without an additional history or FFT copy.
class PreparedFourierTempogram {
public:
    bool prepare(TemporalWindowConfig={},std::span<const float> coefficients={},
                 std::size_t zero_padding=0,bool zero_phase=false,const FourierBackendFactory* =nullptr);
    // Explicit transform length, including source FFT truncation. Ordinary
    // prepare() remains the length+zero_padding convenience contract.
    bool prepare_size(TemporalWindowConfig,std::span<const float> coefficients,std::size_t fft_size,bool zero_phase=false,
                      std::span<const double> reference_coefficients={},const FourierBackendFactory* =nullptr);
    static bool storage_bytes(TemporalWindowConfig,std::span<const float> coefficients,
                              std::size_t fft_size,bool zero_phase,std::size_t& bytes,const FourierBackendFactory* =nullptr) noexcept;
    std::size_t frames(std::size_t count) const noexcept {return window_.frames(count);}
    std::size_t bins() const noexcept {return input_.empty()?0:input_.size()/2+1;}
    bool process_frame(std::span<const float>,std::size_t frame,std::span<float> complex) noexcept;
    // Additional explicit FFT-buffer rotation follows embedding, before FFT.
    bool process_at(std::span<const float>,std::ptrdiff_t first_sample,std::span<float> complex,std::ptrdiff_t rotation=0,
                    TemporalPadding=TemporalPadding::Constant) noexcept;
    bool set_window_coefficients(std::span<const float> values) noexcept {ready_=false;return window_.set_coefficients(values);}
    const TemporalWindowConfig& config() const noexcept {return window_.config();}
    std::span<const float> window_coefficients() const noexcept {return window_.coefficients();}
    FourierTempogramView view() const noexcept;
    std::size_t workspace_bytes() const noexcept;
private:
    bool prepare_transform(std::size_t,const FourierBackendFactory*);
    bool load(std::span<const float>,std::size_t) noexcept;
    bool execute(std::span<float>) noexcept;
    bool prepare_reference(std::size_t);
    bool embed_reference(std::ptrdiff_t rotation=0) noexcept;
    bool execute_reference(std::span<float>) noexcept;
    PreparedTemporalWindow window_;
    PreparedFourierTransform complex_;
    PreparedRfft real_;
    detail::PreparedReferenceFourier reference_fft_;
    PreparedFourierBackend backend_;
    bool real_fft_=false,zero_phase_=false,ready_=false;
    std::vector<float> input_,output_,windowed_;
    std::vector<double> reference_input_,reference_output_,reference_windowed_;
};
// Raw real-domain linear ACF through an explicitly 2/3/5-smooth FFT length.
// A caller may replace this recipe, including supplying the transform length.
SpectralAutocorrelationConfig real_linear_autocorrelation_config() noexcept;
struct AutocorrelationTempogramConfig {
    TemporalWindowConfig window;
    VectorNormalization normalization=VectorNormalization::Maximum;
    float norm_exponent=2;
    SpectralAutocorrelationConfig correlation=real_linear_autocorrelation_config();
    TemporalPadding padding=TemporalPadding::LinearRamp;
};
struct AutocorrelationTempogramView {
    // raw_autocorrelation is the configured correlation output before vector
    // normalization; correlation exposes the unscaled complex inverse too.
    std::span<const float> windowed_input,raw_autocorrelation,normalized_values;
    SpectralAutocorrelationView correlation;
};
// Port of librosa tempogram: linear-ramp padding, periodic window, raw linear
// ACF, then vector normalization. No implicit mean subtraction or lag-energy
// normalization. Output is one complete [lag] column.
class PreparedAutocorrelationTempogram {
public:
    bool prepare(AutocorrelationTempogramConfig={},std::span<const float> coefficients={});
    static bool storage_bytes(AutocorrelationTempogramConfig,std::span<const float> coefficients,std::size_t& bytes) noexcept;
    std::span<const float> window_coefficients() const noexcept {return window_.coefficients();}
    std::size_t frames(std::size_t count) const noexcept;
    bool process_frame(std::span<const float>,std::size_t frame,std::span<float> acf) noexcept;
    const AutocorrelationTempogramConfig& config() const noexcept {return config_;}
    std::size_t fft_size() const noexcept {return correlation_.fft_size();}
    AutocorrelationTempogramView view() const noexcept;
    std::size_t workspace_bytes() const noexcept;
    std::span<const double> reference_values() const noexcept {return ready_?std::span<const double>(reference_output_):std::span<const double>{};}
private:
    bool process_reference(std::span<const float>,std::size_t) noexcept;
    bool process_portable(std::span<const float>,std::size_t) noexcept;
    bool configure(AutocorrelationTempogramConfig,std::span<const float>);
    PreparedTemporalWindow window_;
    PreparedSpectralAutocorrelation correlation_;
    AutocorrelationTempogramConfig config_;
    std::vector<float> input_,output_;
    std::vector<double> reference_input_,reference_output_;
    bool ready_=false;
};
// Explicit BPM grids; lag zero is +infinity. input_rate is the rate of the
// scalar envelope, not PCM Fs. These operations do not select a musical tempo.
bool fourier_tempo_frequencies(float input_rate,std::size_t fft_size,std::span<float>) noexcept;
bool autocorrelation_tempo_frequencies(float input_rate,std::span<float>) noexcept;
// Linear interpolation onto the Fourier grid followed by the geometric mean.
bool hybrid_tempogram_frame(std::span<const float> complex,std::span<const float> acf,
                           float input_rate,std::size_t fft_size,std::span<float>) noexcept;
// Explicit Fourier BPM grid and a prepared ACF row support all interpolation
// kinds, fill/extrapolation settings; source geometric-mean combination.
bool hybrid_tempogram_frame(std::span<const float> fourier_bpm,std::span<const float> complex,
                           const PreparedCurveInterpolation& acf,std::span<float>) noexcept;
}
