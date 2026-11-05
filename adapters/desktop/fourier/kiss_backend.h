#pragma once
#include <dsp_kernels/fourier_backend.h>
namespace desktop_fourier {
// Optional pinned KISS FFT130 Float32 provider. No application/SDK default
// selects it. The returned static factory is borrowed only during preparation.
const dsp_kernels::FourierBackendFactory& kiss_backend_factory() noexcept;
}
