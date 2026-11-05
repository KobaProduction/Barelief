// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dsp_kernels/fourier_backend.h>
namespace desktop_fourier {
// Pinned aubio Float32 Ooura RDFT. Explicit real power-of-two requests only;
// plans own cold-materialized tables/scratch and clone without the factory.
const dsp_kernels::FourierBackendFactory& ooura_backend_factory() noexcept;
}
