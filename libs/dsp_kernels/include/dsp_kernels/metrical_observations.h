#pragma once
#include <dsp_kernels/metrical_state_space.h>
#include <cstdint>
#include <span>

namespace dsp_kernels {
enum class MetricalObservation {BarStart,BeatAndDownbeat};
// Source observation pointers. BarStart uses position<1/lambda, NOT position%1.
// BeatAndDownbeat uses 0=nonbeat,1=beat,2=downbeat, with source strict borders.
bool metrical_observation_pointers(const PreparedMetricalStateSpace&,float lambda,MetricalObservation,
                                   std::span<std::uint32_t>) noexcept;
// Primitive observation formula shared by existing beat and new bar decoders.
// Caller validates probability/lambda. Zero probability maps to -inf in log mode.
float beat_activation_density(float activation,float lambda,bool active,bool logarithmic) noexcept;
bool downbeat_activation_log_densities(std::span<const float> beat_downbeat,float lambda,
                                       std::span<float> output) noexcept;
}
