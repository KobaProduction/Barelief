#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <memory>

namespace dsp_graph_adapters {
class ClassicalModelProvider;
struct DspOperationFactoryOptions {
    std::shared_ptr<const dataflow_runtime::NumericOperationFactory> fallback;
    std::shared_ptr<const ClassicalModelProvider> models;
};
// Composition port: common numerical operations, source sequences, then the
// injected optional factory. No graph executor or platform/model I/O lives here.
std::shared_ptr<const dataflow_runtime::NumericOperationFactory>
make_dsp_operation_factory(DspOperationFactoryOptions={});
}
