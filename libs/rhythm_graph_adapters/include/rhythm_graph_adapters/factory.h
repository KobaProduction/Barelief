// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <dsp_kernels/prepared_resampler.h>

namespace rhythm_graph_adapters {
struct RhythmOperationFactoryOptions {
    std::shared_ptr<const dataflow_runtime::NumericOperationFactory> fallback;
    // A prepared predictive tracker/clone pins this provider; no desktop table
    // or unowned callback is hidden in the portable source bindings.
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> window_resampler;
};
std::shared_ptr<const dataflow_runtime::NumericOperationFactory>
make_rhythm_operation_factory(RhythmOperationFactoryOptions={});
}
