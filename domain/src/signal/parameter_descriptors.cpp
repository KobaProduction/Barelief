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
        {"radix",C::Processing,V::String,0,0,0,"radix2"}};
constexpr ParameterDescriptor comparison[]{
        {"window_samples",C::Processing,V::Integer,1,65536,512,{}},
        {"silence_threshold",C::Processing,V::Number,0,1,1e-12,{}}};
constexpr ParameterDescriptor merge[]{{"merge",C::Processing,V::Integer,0,2,2,{}}};
constexpr ParameterDescriptor spectral_weight_grid[]{
        {"sample_rate",C::Processing,V::Integer,1,4294967295.,48000,{}},
        {"bin_hz",C::Processing,V::Number,1e-9,4294967295.,1,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.,1,{}},
        {"frequencies_hz",C::Processing,V::NumericArray,0,32768,0,{}}};
constexpr auto mel=[] {
    std::array<ParameterDescriptor,5> fields{};
    fields[0]={"bands",C::Processing,V::Integer,1,50,50,{}};
    for(std::size_t i=0;i<4;++i)fields[i+1]=spectral_weight_grid[i];
    return fields;
}();
constexpr ParameterDescriptor window[]{
        {"size",C::Processing,V::Integer,2,4294967295.,4096,{}},
        {"input_rate",C::Processing,V::Integer,0,4294967295.,0,{}}};
constexpr ParameterDescriptor function[]{{"function",C::Processing,V::WindowFunction,0,0,0,"hann"},{"size",C::Processing,V::Integer,1,4294967295.,4096,{}}};
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
        {"positive_half",C::Processing,V::Boolean,0,1,1,{}},
        {"normalize",C::Processing,V::Boolean,0,1,1,{}},
        {"coherent_gain",C::Processing,V::Number,1e-15,1e15,1,{}},
        {"power_gain",C::Processing,V::Number,1e-15,1e15,1,{}},
        {"sample_rate",C::Processing,V::Integer,1,4294967295.0,48000,{}},
        {"representation",C::Processing,V::String,0,0,0,"peak_amplitude"}};
constexpr ParameterDescriptor window_gain[]{ {"function",C::Processing,V::WindowFunction,0,0,0,"hann"},{"power",C::Processing,V::Boolean,0,1,0,{}} };
constexpr ParameterDescriptor frequency_step[]{{"fft_size",C::Processing,V::Integer,1,4294967295.0,4096,{}}};
constexpr ParameterDescriptor spectrum_peak[]{
    {"bin_hz",C::Processing,V::Number,1e-15,4294967295.,1,{}},
    {"first_bin",C::Processing,V::Integer,0,4294967295.,0,{}}};
constexpr ParameterSet sets[]{
    {NodeKind::FrequencyStep,frequency_step},
    {NodeKind::SpectrumPeak,spectrum_peak},
    {NodeKind::SpectrumAmplitude,amplitude},
    {NodeKind::Fourier,rfft},
    {NodeKind::RealFourier,rfft},
    {NodeKind::RFFT,rfft},
    {NodeKind::Fs,fs},
    {NodeKind::AmplitudePhase,amplitude_phase},
    {NodeKind::WindowGain,window_gain},
    {NodeKind::PcmResampler,resampler},
    {NodeKind::ChannelComparison,comparison},
    {NodeKind::SpectrumMerge,merge},
    {NodeKind::SpectrumGain,gain},
    {NodeKind::BarkWeighting,spectral_weight_grid},
    {NodeKind::Mel,mel},
    {NodeKind::Window,window},
    {NodeKind::WindowFunction,function},
    {NodeKind::TestSignal,signal},
};
}
std::span<const ParameterDescriptor> signal_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
