// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <span>
#include <cstddef>
#include <dsp_kernels/numerical_precision.h>

namespace rhythm_reference_ports {
enum class SpectralWindowShape {Hann,Hamming,CenteredHann,Triangular,Rectangular,
                                BlackmanHarris62,BlackmanHarris70,BlackmanHarris74,BlackmanHarris92};
struct SpectralWindowConfig {
    SpectralWindowShape shape=SpectralWindowShape::Hann;
    std::size_t constants_decimals=5;
    bool symmetric=true,normalized=true;
    dsp_kernels::NumericalPrecision precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
// Source spectral-window recipes, with explicit coefficients/periodicity.
// PortableFloat32 evaluates coefficients/trigonometry in Float32. Explicit
// DesktopReference uses the original Double expression before each Float32
// store. Both normalize by accumulating and multiplying in Float32.
// Normalized means sum(abs(window))=2, not peak=1 or energy=1.
bool normalize_spectral_window(std::span<float> values) noexcept;
bool spectral_window(std::span<float> output,SpectralWindowConfig={}) noexcept;
// Validates source geometry/settings without preparing coefficients. This
// operation owns no heap payload: output storage belongs to the caller.
bool spectral_window_storage_bytes(std::size_t samples,SpectralWindowConfig,std::size_t&) noexcept;
}
