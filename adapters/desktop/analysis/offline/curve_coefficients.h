#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <string>

namespace desktop_analysis::offline {
// Evaluate a saved frequency function on an explicit FFT grid and render a
// portable constexpr float table. The caller owns the output file location.
bool frequency_coefficients_cpp_header(const dsp_kernels::FrequencyProfile& source,
    const dsp_kernels::ProfileCompatibility& target,std::string& header,std::string& error);
}
