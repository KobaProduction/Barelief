#pragma once
#include <ooura_backend.h>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
namespace source_fourier_profile {
// Explicit test composition only. Essentia/BTrack selectors and all portable
// defaults remain independent; a profile is never inferred from a library ID.
inline const dsp_kernels::FourierBackendFactory* aubio() {
    const char* value=std::getenv("BARELIEF_AUBIO_SOURCE_FFT_BACKEND");
    if(!value || std::string_view(value)=="default")return nullptr;
    if(std::string_view(value)!="aubio_ooura_fft8g_float32")throw std::runtime_error("unknown explicit Aubio Fourier profile");
    return &desktop_fourier::ooura_backend_factory();
}
}
