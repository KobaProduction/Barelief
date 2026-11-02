#pragma once
namespace dsp_kernels {
// Explicit arithmetic policy; external samples and published stage views stay
// Float32. DesktopReference retains the original Double window/FFT/ACF seams.
// PortableFloat32 remains the default and the future optimization baseline.
enum class NumericalPrecision {PortableFloat32,DesktopReference};
}
