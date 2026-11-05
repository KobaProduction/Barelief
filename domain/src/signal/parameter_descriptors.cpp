#include "../graph/parameter_sets.h"
#include <limits>
namespace domain::detail {
namespace {
constexpr ParameterDescriptor fs[]{{"value",C::Processing,V::Integer,8000,192000,48000,{}}};
constexpr ParameterDescriptor resampler[]{
        {"input_rate",C::Processing,V::Integer,0,4294967295.,0,{}},
        {"output_rate",C::Processing,V::Integer,8000,192000,48000,{}},
        {"mode",C::Processing,V::String,0,0,0,"existing_fir"},
        {"quality",C::Processing,V::Integer,0,10,5,{}}};
constexpr ParameterDescriptor rfft[]{
        {"fft_size",C::Processing,V::Integer,2,4294967295.,4096,{}},
        {"sample_rate",C::Processing,V::Integer,1,4294967295.,48000,{}},
        {"radix",C::Processing,V::String,0,0,0,"radix2"}};
constexpr ParameterDescriptor raw_fourier[]{
        {"fft_size",C::Processing,V::Integer,4,4294967295.,4096,{}},
        {"radix",C::Processing,V::String,0,0,0,"radix2"},
        {"backend_id",C::Processing,V::String,0,0,0,{}}};
constexpr ParameterDescriptor real_fourier[]{
        {"fft_size",C::Processing,V::Integer,8,4294967295.,4096,{}},raw_fourier[1],raw_fourier[2],
        {"include_endpoints",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor comparison[]{
        {"window_samples",C::Processing,V::Integer,1,65536,512,{}},
        {"silence_threshold",C::Processing,V::Number,0,1,1e-12,{}}};
constexpr ParameterDescriptor merge[]{{"merge",C::Processing,V::Integer,0,2,2,{}}};
constexpr ParameterDescriptor spectral_weight_grid[]{
        {"sample_rate",C::Processing,V::Integer,1,4294967295.,48000,{}},
        {"bin_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}},
        {"frequencies_hz",C::Processing,V::NumericArray,0,32768,0,{}}};
constexpr ParameterDescriptor mel[]{
    {"bands",C::Processing,V::Integer,1,4294967295.,50,{}},
    {"sample_rate",C::Processing,V::Integer,1,4294967295.,48000,{}},
    {"fft_size",C::Processing,V::Integer,0,4294967295.,0,{}},
    {"bin_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}},
    {"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}},
    {"frequencies_hz",C::Processing,V::NumericArray,0,4294967295.,0,{}},
    {"scale",C::Processing,V::String,0,0,0,"htk"},
    {"fmin_hz",C::Processing,V::Number,0,1e12,0,{}},
    {"fmax_hz",C::Processing,V::Number,0,1e12,0,{}},
    {"norm",C::Processing,V::String,0,0,0,"none"},
    {"norm_p",C::Processing,V::Number,1e-12,1e12,2,{}}};
constexpr ParameterDescriptor window[]{
        {"size",C::Processing,V::Integer,1,4294967295.,4096,{}}};
constexpr ParameterDescriptor frames[]{
        {"size",C::Processing,V::Integer,1,4294967295.,4096,{}},
        {"hop_samples",C::Processing,V::Integer,1,4294967295.,512,{}}};
constexpr ParameterDescriptor stride[]{{"every_n",C::Processing,V::Integer,1,4294967295.,1,{}},
    {"materialize",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor function[]{{"function",C::Processing,V::WindowFunction,0,0,0,"hann"},{"size",C::Processing,V::Integer,1,4294967295.,4096,{}},{"periodic",C::Processing,V::Boolean,0,1,0,{}},{"precision",C::Processing,V::Integer,0,1,0,{}}};
constexpr ParameterDescriptor signal[]{
        {"tones",C::Processing,V::Integer,5,10,8,{}},{"sample_rate",C::Processing,V::Integer,8000,192000,48000,{}},
        {"minimum_hz",C::Processing,V::Number,10,96000,73,{}},{"maximum_hz",C::Processing,V::Number,10,96000,13901,{}},
        {"amplitude",C::Processing,V::Number,0,1,.4,{}},{"period_seconds",C::Processing,V::Number,.1,3600,6,{}},
        {"sweep_depth",C::Processing,V::Number,0,.95,.18,{}}};
constexpr ParameterDescriptor amplitude[]{
        {"fft_size",C::Processing,V::Integer,1,4294967295.0,4096,{}},
        {"positive_half",C::Processing,V::Boolean,0,1,1,{}},
        {"coherent_gain",C::Processing,V::Number,1e-15,1e15,1,{}}};
constexpr ParameterDescriptor amplitude_phase[]{
        {"fft_size",C::Processing,V::Integer,1,4294967295.0,4096,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}},
        {"magnitude_norm",C::Processing,V::String,0,0,0,"hypot"},
        {"zero_phase",C::Processing,V::String,0,0,0,"zero"},
        {"real_endpoints",C::Processing,V::Boolean,0,1,0,{}},
        {"positive_half",C::Processing,V::Boolean,0,1,1,{}},
        {"normalize",C::Processing,V::Boolean,0,1,1,{}},
        {"window_coefficients",C::Processing,V::NumericArray,0,32768,0,{}},
        {"sample_rate",C::Processing,V::Integer,0,4294967295.0,0,{}},
        {"representation",C::Processing,V::String,0,0,0,"peak_amplitude"}};
constexpr ParameterDescriptor complex_polar[]{
        {"fft_size",C::Processing,V::Integer,1,4294967295.,4096,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.,0,{}},
        {"positive_half",C::Processing,V::Boolean,0,1,0,{}},
        {"magnitude_norm",C::Processing,V::String,0,0,0,"hypot"},
        {"zero_phase",C::Processing,V::String,0,0,0,"zero"},
        {"real_endpoints",C::Processing,V::Boolean,0,1,0,{}}};
constexpr ParameterDescriptor window_gain[]{ {"function",C::Processing,V::WindowFunction,0,0,0,"hann"},{"power",C::Processing,V::Boolean,0,1,0,{}},{"periodic",C::Processing,V::Boolean,0,1,0,{}} };
constexpr ParameterDescriptor frequency_step[]{{"fft_size",C::Processing,V::Integer,1,4294967295.0,4096,{}}};
constexpr ParameterDescriptor spectrum_peak[]{
    {"bin_hz",C::Processing,V::Number,1e-15,4294967295.,1,{}},
    {"first_bin",C::Processing,V::Integer,0,4294967295.,0,{}}};
constexpr ParameterDescriptor power_to_db[]{
    {"reference_mode",C::Processing,V::String,0,0,0,"value"},
    {"reference",C::Processing,V::Number,-1e300,1e300,1,{}},
    {"amin",C::Processing,V::Number,1e-45,3.4e38,1e-10,{}},
    {"top_db",C::Processing,V::Number,0,3.4e38,80,{}},
    {"top_db_enabled",C::Processing,V::Boolean,0,1,1,{}},
    {"reduction_axes",C::Processing,V::String,0,0,0,"all"}};
constexpr ParameterSet sets[]{
    {NodeKind::PowerToDb,power_to_db},
    {NodeKind::FrequencyStep,frequency_step},
    {NodeKind::SpectrumPeak,spectrum_peak},
    {NodeKind::SpectrumAmplitude,amplitude},
    {NodeKind::Fourier,raw_fourier},
    {NodeKind::RealFourier,real_fourier},
    {NodeKind::RFFT,rfft},
    {NodeKind::Fs,fs},
    {NodeKind::AmplitudePhase,amplitude_phase},
    {NodeKind::ComplexPolar,complex_polar},
    {NodeKind::WindowGain,window_gain},
    {NodeKind::PcmResampler,resampler},
    {NodeKind::ChannelComparison,comparison},
    {NodeKind::SpectrumMerge,merge},
    {NodeKind::SpectrumGain,gain},
    {NodeKind::BarkWeighting,spectral_weight_grid},
    {NodeKind::Mel,mel},
    {NodeKind::Window,window},
    {NodeKind::FrameBuffer,frames},
    {NodeKind::FrameStride,stride},
    {NodeKind::WindowFunction,function},
    {NodeKind::TestSignal,signal},
};
}
std::span<const ParameterDescriptor> signal_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
