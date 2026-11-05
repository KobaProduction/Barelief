// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
namespace rhythm_graph_adapters {
// BTrack() canonical Float32 research graph: 44100 Hz, hop512, N1024.
// The source-specific stages do not own PCM framing, FFT or polar conversion.
signal_contracts::Status make_btrack_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::ProcessingPort>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t metadata_bytes,domain::ConfiguredProcessingNode&);
}
