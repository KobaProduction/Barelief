// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_graph_adapters/profile.h>
#include <utility>
namespace rhythm_graph_adapters {
dataflow_runtime::NumericRuntimeProfile with_reference_operations(dataflow_runtime::NumericRuntimeProfile profile,
    std::shared_ptr<const dsp_graph_adapters::ClassicalModelProvider> models,
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> resampler){
    profile.operation_factory=make_rhythm_operation_factory({profile.operation_factory,std::move(resampler)});
    return dsp_graph_adapters::with_dsp_operations(std::move(profile),std::move(models));
}
}
