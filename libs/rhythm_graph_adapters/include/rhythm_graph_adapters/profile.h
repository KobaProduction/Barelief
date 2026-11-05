// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_graph_adapters/profile.h>
#include <rhythm_graph_adapters/factory.h>
namespace rhythm_graph_adapters {
// Explicit optional PC composition. No implicit application/SDK GPL linkage.
// Order: portable common + classical DSP sequences, optional reference ports,
// then the caller's prior factory. Models/resampler providers remain owned.
dataflow_runtime::NumericRuntimeProfile with_reference_operations(
    dataflow_runtime::NumericRuntimeProfile,
    std::shared_ptr<const dsp_graph_adapters::ClassicalModelProvider> models={},
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> window_resampler={});
}
