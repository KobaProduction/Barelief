#pragma once
#include <domain/graph_definition.h>
#include <cstddef>

namespace domain {
struct GraphLimits { std::size_t max_nodes = 128, max_edges = 256; };
// Explicit adapter capabilities; an empty supported set supports nothing.
struct DeviceCapabilities {
    std::vector<NodeKind> node_kinds;
    std::vector<FrameType> receive_types, publish_types;
    std::size_t max_wled_outputs = 1;
};
DeviceCapabilities desktop_capabilities();
enum class GraphError {
    None, GraphLimit, InvalidNodeId, DuplicateNodeId, UnknownKind, InvalidParameters,
    UnsupportedKind, UnsupportedFrameType, OutputLimit, MissingNodeOrPort,
    IncompatiblePorts, MultipleInputs, Cycle
};
struct ValidationResult {
    GraphError error = GraphError::None;
    NodeId node = -1;
    std::size_t edge_index = 0;
    std::vector<NodeId> topological_order;
    explicit operator bool() const noexcept { return error == GraphError::None; }
};
ValidationResult validate_graph(const GraphDefinition&, const DeviceCapabilities&,
                                GraphLimits limits = {});
}
