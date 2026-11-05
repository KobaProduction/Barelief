#pragma once
#include "native_export_io.h"
#include <kiss_backend.h>
#include <cstdlib>
#include <string_view>
namespace native_export {
// Explicit test-driver profile only. It never changes an application, portable
// default, or another library's recipe. Unknown selectors are rejected.
inline const dsp_kernels::FourierBackendFactory* source_fourier_backend() {
    const char* value=std::getenv("BARELIEF_SOURCE_FFT_BACKEND");
    if(!value || std::string_view(value)=="default")return nullptr;
    require(std::string_view(value)=="kiss_fft130_float32","unknown source Fourier profile");
    return &desktop_fourier::kiss_backend_factory();
}
}
