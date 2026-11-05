// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
namespace rhythm_graph_adapters {
// Explicit source-stage graph composition. No DSP, I/O or preparation occurs
// while deriving output contracts. Required input ports are ordered 0..N-1.
// Every analytical value is Float32; counters/counts are UInt64, states UInt8.
// See batch_operation_contracts.md for the complete finite settings/port map.
signal_contracts::Status make_batch_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,
    domain::ConfiguredProcessingNode& output);
}
