// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
namespace rhythm_reference_ports {
enum class AnalysisWindowShape {Ones,HalfRectangle,PeriodicHamming,PeriodicHann,Blackman,BlackmanHarris,Gaussian,Welch,Parzen};
// aubio phase-vocoder windows; cosine-series math reuses dsp_kernels. Rectangle
// is deliberately constant .5, and periodic Hann/Hamming use denominator N.
// Periodic Hann preserves aubio's mixed cold evaluation: Double angle to cosf,
// then Float32 coefficients. Prepared history, multiplication and FFT stay F32.
bool analysis_window(std::span<float> coefficients,AnalysisWindowShape=AnalysisWindowShape::PeriodicHann) noexcept;
// BTrack's historical "Tukey" formula: alpha=.5, position=n-floor(N/2)+1.
// Kept distinct from a standard symmetric Tukey window. Other BTrack windows
// reuse the common symmetric Hann/Hamming/Blackman/rectangular operations.
bool shifted_cosine_taper(std::span<float> coefficients) noexcept;
enum class PredictiveWindowShape {Rectangle,Hann,Hamming,Blackman,ShiftedTaper};
bool predictive_window(std::span<float>,PredictiveWindowShape=PredictiveWindowShape::Hann) noexcept;
}
