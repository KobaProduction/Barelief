#pragma once
// Opt-in benchmark instrumentation. The firmware composition supplies the clock
// and collector; normal kernels have no clock/platform dependency or overhead.
#if defined(DSP_FFT_PROFILE)
#include <cstdint>
namespace dsp_kernels::detail {
enum class FftStage { Input, Pack, Kernel, Permute, Mixed, Split, Validate, Publish, Count };
std::uint32_t fft_profile_clock() noexcept;
void fft_profile_record(FftStage stage, std::uint32_t cycles) noexcept;
}
#define DSP_FFT_BEGIN(name) const auto fft_start_##name = dsp_kernels::detail::fft_profile_clock()
#define DSP_FFT_END(name) dsp_kernels::detail::fft_profile_record(dsp_kernels::detail::FftStage::name, dsp_kernels::detail::fft_profile_clock() - fft_start_##name)
#else
#define DSP_FFT_BEGIN(name)
#define DSP_FFT_END(name)
#endif
