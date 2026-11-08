// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
namespace rhythm_graph_adapters {
// BTrack() canonical mixed-precision graph: 44100 Hz, hop512, N1024.
// PCM framing, window multiplication and Fourier-domain ACF are explicit graph stages.
signal_contracts::Status make_btrack_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::ProcessingPort>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t metadata_bytes,domain::ConfiguredProcessingNode&);
}
