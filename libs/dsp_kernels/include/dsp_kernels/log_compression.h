#pragma once
#include <span>
namespace dsp_kernels {
enum class LogarithmBase {Ten,Natural,Two};
// madmom LogarithmicSpectrogram operation, independently usable on linear
// magnitudes or filter-bank outputs. This logarithm is not a decibel scale.
// Output = logarithm(add + multiply * input); zero produces -infinity.
// Ten is the original spectrogram default; Natural and Two cover the other
// source compression/log-energy recipes without changing the linear Mel bank.
// Default recipe log10(1+x); caller chooses compression and scale explicitly.
// Disjoint or exactly identical spans; validation precedes output writes.
bool logarithmic_compression(std::span<const float> input,std::span<float> output,
                             float multiply=1,float add=1,LogarithmBase=LogarithmBase::Ten,
                             bool allow_signed_input=false) noexcept;
// aubio's cvec_logmag operation: log(1 + lambda * magnitude).
bool logarithmic_magnitude(std::span<const float> input,std::span<float> output,
                           float lambda) noexcept;
}
