#pragma once
#include "node_state.h"
namespace dataflow_runtime {
struct RuntimeContext::Impl {
    std::vector<detail::State> states;
    const PreparedGraph *plan = nullptr;
};
struct PreparedGraph::Impl {
    application::MemoryRequirements memory;
    std::vector<detail::PlanNode> nodes;
    std::shared_ptr<const domain::NumericProcessingGraph> numeric_graph;
    std::shared_ptr<const domain::NumericProcessingGraph> numeric_source_graph;
    std::shared_ptr<const std::vector<std::byte>> silence;
    NumericRuntimeProfile numeric_profile{};
    std::optional<domain::ProcessingGraphAdmission> numeric_admission;
};
}
