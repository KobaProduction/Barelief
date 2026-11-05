// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
namespace rhythm_graph_adapters {
// Explicit independent whole-buffer batch. No input payload is copied or retained.
signal_contracts::Status make_loudness_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::OwnedProcessingContract>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,
    domain::ConfiguredProcessingNode&);
}
