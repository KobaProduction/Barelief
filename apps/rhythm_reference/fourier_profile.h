// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/graph/processing_model.h>
#include <dsp_kernels/fourier_backend.h>
#include <memory>

// Cold composition only: unnamed transforms keep their builtin implementation.
// A graph can explicitly request one installed named provider, without fallback.
std::shared_ptr<const dsp_kernels::FourierBackendFactory> research_fourier_backend(const domain::NumericProcessingGraph&);
