// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// ContrastPeakPicker/ContrastPeakAccumulator take mono native Float32 vector0
// chunks; optional scalar UInt8 control65500 supplies explicit EOF/reset.
// Float32 settings: frame_rate=172 (>0), threshold=.05 and ratio_threshold=16
// (>=0), combine_ms=30/pre_average_ms=100/pre_maximum_ms=30 (>0). Source
// truncation requires averaging/maximum lengths >=2. Accumulator adds UInt64
// maximum_onsets=4096 (>0); capacity exhaustion fails without dropping events.
// Picker outputs: event-seconds0[0..N], average1/maxima2[actual chunk count],
// cumulative-detections3 UInt64. Accumulator: current accumulated event-seconds0
// [0..maximum_onsets], count1 UInt64. EOF may omit frame0; a supplied final chunk
// is consumed once, then source finish emits and resets the accumulator.
// Event0 has no PCM clock/sample interval; diagnostics retain current0 clock.
// All payload axes are indices. Histories/counters follow original source
// reset and consumed-prefix failure rules; no short-packet EOF or hidden flush.
signal_contracts::Status make_contrast_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
