// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <span>
namespace rhythm_reference_ports {
enum class NoveltyWeight {Flat,Triangle,InverseTriangle,Parabola,InverseParabola,
    Linear,Quadratic,InverseQuadratic,Supplied,Hybrid};
// Source frequency-band weighting, including historical odd-centre values.
// Hybrid is a product of four summed curves, not one weight vector, and is
// therefore rejected here. Supplied coefficients are explicit caller data.
bool novelty_band_weights(std::span<float> output,NoveltyWeight,
                          std::span<const float> supplied={}) noexcept;
}
