#pragma once
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/fourier_transform.h>
#include <dsp_kernels/detail/prepared_reference_fourier.h>
#include <dsp_kernels/numerical_precision.h>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class AutocorrelationInverseOutput {Real,ComplexMagnitude};
enum class AutocorrelationTransform {Automatic,Complex,Real};
enum class AutocorrelationFftLength {PowerOfTwo,Smooth235};
struct SpectralAutocorrelationConfig {
    std::size_t fft_size=0;
    float spectrum_divisor=1,spectral_exponent=2,inverse_scale=1;
    CorrelationNormalization normalization=CorrelationNormalization::Raw;
    AutocorrelationInverseOutput inverse_output=AutocorrelationInverseOutput::Real;
    AutocorrelationTransform transform=AutocorrelationTransform::Automatic;
    AutocorrelationFftLength length=AutocorrelationFftLength::PowerOfTwo;
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
// An explicit fft_size overrides the policy. Otherwise PowerOfTwo uses the
// smallest 2^k >= 2*N; Smooth235 uses the smallest 2^a*3^b*5^c >= 2*N-1.
bool autocorrelation_fft_size(std::size_t samples,const SpectralAutocorrelationConfig&,
                              std::size_t& output) noexcept;
struct SpectralAutocorrelationView {
    // Spectrum is N/2+1 complex bins in Real mode (or native RFFT), otherwise
    // N complex bins. Power/inverse retain full N-bin geometry; values are lags.
    std::span<const float> padded_input,complex_spectrum,power_spectrum,complex_inverse,values;
    std::size_t input_samples=0,fft_size=0;
};
// Explicit Fourier-domain autocorrelation: zero-pad, transform, compress
// magnitude, inverse transform, then optional overlap/full-length division.
// All scaling is supplied; inverse_scale=1 retains the normalized IDFT.
// Real/ComplexMagnitude selects the inverse result before scaling/division.
// Automatic uses RFFT when supported; Complex retains full-transform source
// arithmetic for recipes requiring that path (including BTrack).
// Real enforces Hermitian compression even when an arbitrary-length complex
// backend implements the real transform. No transform length is substituted.
// No mean removal, temporal window or input history is implicit.
class PreparedSpectralAutocorrelation {
public:
    bool prepare(std::size_t samples,SpectralAutocorrelationConfig={},const FourierBackendFactory* =nullptr);
    static bool storage_bytes(std::size_t samples,SpectralAutocorrelationConfig,std::size_t& bytes,const FourierBackendFactory* =nullptr) noexcept;
    bool process(std::span<const float> input,std::span<float> output) noexcept;
    // Same prepared transform, with a shorter explicitly zero-padded input;
    // Unbiased/Biased denominators use its ACTUAL sample count.
    bool process_prefix(std::span<const float> input,std::span<float> output) noexcept;
    // Explicit local-Double route for source pipelines; regular public views
    // remain rounded Float32. Only real-domain transforms are supported here.
    bool process_double(std::span<const double>,std::span<double>) noexcept;
    std::size_t samples() const noexcept {return samples_;}
    std::size_t fft_size() const noexcept {return config_.fft_size;}
    const SpectralAutocorrelationConfig& config() const noexcept {return config_;}
    // Borrowed intermediate values, valid until the next prepare/process call.
    // Empty after any failed call. Complex arrays use interleaved Re/Im pairs.
    SpectralAutocorrelationView view() const noexcept;
    std::size_t workspace_bytes() const noexcept;
private:
    bool prepare_transforms(const SpectralAutocorrelationConfig&,const FourierBackendFactory*);
    bool configure(std::size_t,SpectralAutocorrelationConfig,const FourierBackendFactory*);
    bool configure_backend(const FourierBackendFactory&);
    bool inverse_backend() noexcept;
    bool forward(std::span<const float>) noexcept;
    bool compress() noexcept;
    bool inverse() noexcept;
    float compressed(float real,float imaginary) const noexcept;
    float inverse_value(std::size_t lag) const noexcept;
    bool collect(std::size_t samples,std::size_t lags) noexcept;
    bool configure_reference();
    bool process_reference(std::span<const float>,std::span<float>) noexcept;
    bool forward_reference(std::span<const double>) noexcept;
    bool compress_reference() noexcept;
    bool collect_reference(std::size_t,std::size_t) noexcept;
    bool publish_reference(std::size_t) noexcept;
    PreparedRfft real_fft_;
    PreparedFourierTransform complex_fft_;
    detail::PreparedReferenceFourier reference_fft_;
    PreparedFourierBackend backend_forward_,backend_inverse_;
    SpectralAutocorrelationConfig config_;
    std::size_t samples_=0,active_samples_=0,active_lags_=0;
    bool use_real_=false,hermitian_=false,ready_=false;
    std::vector<float> input_,transformed_,spectrum_,result_,values_;
    std::vector<double> reference_input_,reference_complex_,reference_spectrum_,reference_result_,reference_values_;
    std::vector<float> backend_spectrum_,backend_result_;
};
// A prepared family of next-power-of-two FFT ACF plans. For every actual input
// length selects exactly nextPowerTwo(2*N), rather than using the longest FFT
// or a direct-summation substitute. All plans/storage are cold-prepared.
class PreparedVariableAutocorrelation {
public:
    bool prepare(std::size_t maximum_samples,CorrelationNormalization=CorrelationNormalization::Raw,const FourierBackendFactory* =nullptr);
    static bool storage_bytes(std::size_t maximum_samples,CorrelationNormalization,std::size_t& bytes,const FourierBackendFactory* =nullptr) noexcept;
    bool process(std::span<const float>,std::span<float>) noexcept;
    std::size_t active_fft_size() const noexcept {return active_fft_;}
    std::size_t workspace_bytes() const noexcept;
private:
    bool prepare_plans(std::size_t,CorrelationNormalization,const FourierBackendFactory*);
    std::size_t maximum_samples_=0,active_fft_=0;
    std::vector<PreparedSpectralAutocorrelation> plans_;
};
}
