#pragma once
#include <domain/graph/processing_model.h>
#include <signal_contracts/window_policy.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace domain {
// Neutral settings/geometry; numerical plans and mathematical modes are mapped
// by the runtime adapter, never obtained from upstream metadata or topology.
struct TemporalOperationParameters {
    double input_rate=120,input_origin_s=0;
    std::size_t samples=512,lag_count=512,window_length=384,hop=1,frame_index=0,fft_size=0;
    std::int64_t first_sample=0,rotation=0;
    signal_contracts::WindowFunction function=signal_contracts::WindowFunction::Hann;
    bool periodic=true,center=true,odd_center_ceil=false,empty_centered_frame=false,use_first_sample=false,zero_phase=false;
    std::size_t tail=0,padding=0,fft_length=1,transform=2,correlation_normalization=0,inverse_output=0;
    float spectrum_divisor=1,spectral_exponent=2,inverse_scale=1;
    std::size_t normalization=1,method=0,interpolation_kind=0,spline_order=1;
    float norm_exponent=2,fill_below=0,fill_above=0;
    bool extrapolate=false,bounds_error=false,keep_infinite_coordinates=false,source_linear=false;
    std::span<const float> coefficients;
};
bool temporal_operation_kind(std::string_view) noexcept;
std::span<const ParameterDescriptor> temporal_operation_parameters(std::string_view) noexcept;
bool temporal_operation_settings(const ResolvedProcessingNode&,TemporalOperationParameters&) noexcept;
// Stable output ports, all Float32/mono. Complex rows are [bin][Re,Im].
// TemporalFrame: windowed, coefficients.
// SpectralAutocorrelation: lags, complex spectrum, compressed spectrum,
// complex inverse, padded input, BPM(lag>=1).
// FourierTempogram: complex bins, windowed input, FFT input, BPM.
// AutocorrelationTempogram: normalized lags, pre-vector-normalized lags,
// windowed input, BPM(lag>=1).
// TempogramSequence: selected scalar/real matrix, raw ACF, normalized ACF,
// Fourier Re, Fourier Im, ACF-window, Fourier-window, selected BPM.
// Sequence Fourier uses ports3/4 for the complete complex result; port0=Re.
// Unselected branches are typed zeros. Matrix layout is [frame][lag/bin].
// Coordinates are finite Seconds/Hz; BPM is separate Values payload. Its ACF
// vector omits lag0, while raw ACF retains lag0. Clock stamps pass through.
std::size_t temporal_operation_output_count(std::string_view) noexcept;
signal_contracts::Status temporal_operation_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::vector<OwnedProcessingContract>&) noexcept;
// Structural upper bounds only: automatic FFT lengths are resolved by the
// numerical plan. No fast-length arithmetic is duplicated in this layer.
std::size_t temporal_frame_count(const TemporalOperationParameters&,std::size_t samples) noexcept;
std::size_t temporal_fft_bound(const TemporalOperationParameters&,std::string_view kind) noexcept;
bool temporal_input_contract(const signal_contracts::NumericContract&,const TemporalOperationParameters&,
                             std::string_view kind) noexcept;
}
