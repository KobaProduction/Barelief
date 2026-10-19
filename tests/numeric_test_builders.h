#pragma once

#include <domain/graph/processing_model.h>
#include <signal_contracts/data_frame.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace numeric_test_builders {
using Check = void (*)(bool, const char*);

inline domain::OwnedProcessingContract contract(
    const signal_contracts::DataContract& value,
    const signal_contracts::FrameLimits& limits, std::size_t maximum_bytes,
    Check check, const char* reason) {
    domain::OwnedProcessingContract owned;
    check(domain::own_processing_contract(value, limits, maximum_bytes, owned)
              == signal_contracts::Status::ok, reason);
    return owned;
}

inline domain::OwnedProcessingContract contract(
    const signal_contracts::NumericContract& value,
    const signal_contracts::FrameLimits& limits, std::size_t maximum_bytes,
    Check check, const char* reason) {
    return contract(signal_contracts::DataContract{value}, limits,
                    maximum_bytes, check, reason);
}

inline domain::ProcessingPort port(
    domain::PortId id, domain::PortDirection direction,
    domain::OwnedProcessingContract owned) {
    return {id, direction, std::move(owned)};
}

inline domain::ProcessingPort port(
    domain::PortId id, domain::PortDirection direction,
    const signal_contracts::NumericContract& value,
    const signal_contracts::FrameLimits& limits, std::size_t maximum_bytes,
    Check check, const char* reason) {
    return port(id, direction, contract(value, limits, maximum_bytes, check, reason));
}

inline domain::ConfiguredProcessingNode node(
    domain::NodeId id, std::string kind, domain::ProcessingParameters parameters,
    std::vector<domain::ProcessingPort> inputs,
    std::vector<domain::ProcessingPort> outputs, bool enabled = true) {
    return {id, std::move(kind), std::move(parameters), std::move(inputs),
            std::move(outputs), enabled};
}

inline domain::Edge edge(domain::NodeId from, domain::PortId output,
                         domain::NodeId to, domain::PortId input) {
    return {from, to, output, input};
}

inline domain::NumericProcessingGraph graph(
    std::string id, signal_contracts::Ratio execution_interval,
    std::vector<domain::ConfiguredProcessingNode> nodes,
    std::vector<domain::Edge> edges) {
    domain::NumericProcessingGraph result;
    result.graph_id = std::move(id);
    result.execution_interval = execution_interval;
    result.nodes = std::move(nodes);
    result.edges = std::move(edges);
    return result;
}

// The returned view borrows contract, storage and layouts. Consume it synchronously.
inline signal_contracts::DataFrameView frame(
    signal_contracts::FrameStamp stamp,
    const signal_contracts::NumericContract& contract,
    std::span<const std::byte> storage,
    std::span<const signal_contracts::PlaneLayout> planes,
    std::array<std::size_t, 2> actual,
    signal_contracts::CoordinateView coordinates = {}) {
    return {stamp, signal_contracts::NumericView{
                       &contract, storage, planes, actual, {}, coordinates}};
}
} // namespace numeric_test_builders
