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
    std::size_t content_owned_bytes=0,content_token_count=0;
    std::shared_ptr<const domain::NumericProcessingGraph> numeric_graph;
    std::shared_ptr<const domain::NumericProcessingGraph> numeric_source_graph;
    std::vector<domain::RoutingAlias> routing_aliases;
    std::shared_ptr<const std::vector<std::byte>> silence;
    NumericRuntimeProfile numeric_profile{};
    std::optional<domain::ProcessingGraphAdmission> numeric_admission;
};
}
