// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Cold graph composition helper, not an executor. Parameters are owned values;
// input is an explicit contract. It derives source output capacities and an
// index axis, never musical timing/coefficients from upstream frame metadata.
// Supported kinds match the independent interval/scaling factory bindings.
signal_contracts::Status make_interval_operation_node(domain::NodeId,std::string_view kind,
    const domain::OwnedProcessingContract& input,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
// Source per-frame operations. Required magnitude0 and optional phase1 are
// separate fixed-length vectors; warm control ports are explicit. Outputs
// preserve input0's declared frame clock, with index-only payload axes.
signal_contracts::Status make_spectral_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
