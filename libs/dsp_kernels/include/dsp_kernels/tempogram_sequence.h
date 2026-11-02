// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/tempogram.h>
#include <dsp_kernels/numeric_rows.h>
namespace dsp_kernels {
enum class TempogramMethod {Autocorrelation,Fourier,Hybrid};
struct TempogramSequenceConfig {
    TempogramMethod method=TempogramMethod::Autocorrelation;
    float frame_rate=44100.f/512.f;
    AutocorrelationTempogramConfig autocorrelation;
    CurveInterpolationConfig interpolation;
};
struct TempogramFrameView {
    std::span<const float> values,coordinates_bpm,autocorrelation_bpm,fourier_bpm;
    AutocorrelationTempogramView autocorrelation;
    FourierTempogramView fourier;
    std::size_t frame_index=0;
    // Nominal frame*hop/rate grid origin, before centering offsets/padding.
    double frame_origin_seconds=0;
    bool complex_output=false;
};
// Complete envelope -> temporal representation. Default window is periodic
// Hann, hop1, centered. ACF uses linear-ramp padding, Fourier uses constants;
// hybrid drops infinite lag0 from interpolation and aligns both frame counts.
// Fourier rows are [bin][Re,Im]; ACF/hybrid rows are scalar lag/frequency values.
// Processing uses shared prepared window/FFT/ACF/interpolation operations.
class PreparedTempogramSequence {
public:
    bool prepare(TempogramSequenceConfig={},std::span<const float> coefficients={});
    static bool storage_bytes(TempogramSequenceConfig,std::span<const float> coefficients,std::size_t& bytes) noexcept;
    std::size_t frames(std::size_t input_frames) const noexcept;
    static bool frame_count(TempogramSequenceConfig,std::size_t input_frames,std::size_t&) noexcept;
    std::size_t columns() const noexcept {return row_.size();}
    bool complex_output() const noexcept {return config_.method==TempogramMethod::Fourier;}
    std::span<const float> coordinates_bpm() const noexcept {return coordinates_;}
    const TempogramSequenceConfig& config() const noexcept {return config_;}
    // All views are owned by this prepared instance and borrowed until its
    // next prepare/process call. Empty after failure; row callbacks may inspect
    // this view synchronously. Complex rows have two values per BPM coordinate.
    TempogramFrameView view() const noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool process_frame(std::span<const float>,std::size_t,std::span<float>) noexcept;
    bool process(std::span<const float>,FloatMatrixDestination) noexcept;
    bool process_channels(std::span<const std::span<const float>>,FloatMatrixBatchDestination) noexcept;
private:
    bool prepare_reference_hybrid();
    bool combine_reference_hybrid() noexcept;
    bool interpolate_reference(double,double&) const noexcept;
    bool configure(TempogramSequenceConfig,std::span<const float>);
    bool evaluate(std::span<const float>,std::size_t) noexcept;
    bool prepare_autocorrelation(std::span<const float>);
    bool prepare_fourier(std::span<const float>);
    bool combine() noexcept;
    TempogramSequenceConfig config_;
    PreparedFourierTempogram fourier_;
    PreparedAutocorrelationTempogram autocorrelation_;
    PreparedCurveInterpolation interpolation_;
    std::vector<double> reference_coordinates_;
    std::vector<float> fourier_row_,acf_row_,coordinates_,acf_coordinates_,row_;
    std::size_t active_frame_=0;
    bool ready_=false;
};
}
