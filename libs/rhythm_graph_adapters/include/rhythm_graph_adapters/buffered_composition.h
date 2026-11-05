// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// BufferedMaximum/CausalAverage consume an explicit nonempty vector chunk.
// BufferedBandFlux consumes current0 plus required held delayed1 band row;
// caller supplies the delay. It does not infer history or topology. Output
// ports are novelty0, filtered-reference1, positive-differences2. Index axes
// and current0 frame clock are explicit; arithmetic failure preserves the
// source's consumed prefix, whereas rejected preflight consumes nothing.
// UInt64 settings: Maximum width=3 (>=2), Bool causal=true; Average length=5
// (>0); Flux bin_width=3 (>=3), frame_width=2 (>0), maximum_frames=3
// (>frame_width). Flux's two rows are fixed equal length; current/delayed
// coordinate grids and session identity agree, timestamps/sequences may differ.
// Maximum/Average output0 follows the input chunk's actual count and bounds.
// Optional scalar UInt8 control65500; no chunk-length-based EOF. Reset follows
// original reset policy (Flux retains its child maximum filter); reprepare
// explicitly declares reset and cold cloning deep-copies all source state.
signal_contracts::Status make_buffered_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
