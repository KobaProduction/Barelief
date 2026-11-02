#include <dsp_graph_adapters/profile.h>
#include <utility>
namespace dsp_graph_adapters {
dataflow_runtime::NumericRuntimeProfile with_dsp_operations(dataflow_runtime::NumericRuntimeProfile profile,
    std::shared_ptr<const ClassicalModelProvider> models){
    profile.operation_factory=make_dsp_operation_factory({profile.operation_factory,std::move(models)});
    profile.music_enabled=true;return profile;
}
}
