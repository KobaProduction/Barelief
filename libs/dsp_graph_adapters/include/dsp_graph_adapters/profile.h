#pragma once
#include <dsp_graph_adapters/factory.h>
#include <dataflow_runtime/runtime.h>
namespace dsp_graph_adapters {
// Cold opt-in composition for the existing GraphExecutor. Caller supplies the
// resource limits; this helper never silently increases admission budgets.
// Existing factory is the explicit final fallback; classical models are owned.
dataflow_runtime::NumericRuntimeProfile with_dsp_operations(
    dataflow_runtime::NumericRuntimeProfile,std::shared_ptr<const ClassicalModelProvider> models={});
}
