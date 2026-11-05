// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Scalar decision/picker stages and explicit PCM silence reduction. Source
// counters are UInt64 data, not Float32 analytics or inferred input timestamps.
// AdaptivePeakPicker: Float32 novelty0; UInt64 pre=1/post=5/statistic=0
// (median/mean), threshold=.1, optional Float32 feedforward/feedback arrays[3].
// Outputs: position0 Float32, peak1 UInt8, thresholded2/mean3/statistic4 Float32.
// OnsetDecision: Float32 peak-position0 and UInt8 silence1 (0/1), matched
// frame/session; UInt64 hop_samples=512 (1..UINT32_MAX), delay_samples=2201,
// minimum_interval_samples=2205. Outputs: position0 Float32, local sample1
// UInt64, detected2 UInt8. Local counter starts at0 on explicit reset; input
// sample_begin never changes it. PcmSilence: explicit mono native Float32 PCM
// vector0, finite threshold_db=-70; output0 UInt8. No playback/FFT is involved.
// Optional UInt8 control65500; declared input0 clock is retained per output,
// payload axes are indices, never a time axis for a counter value. Full view,
// physical contract, provenance and all values validate before state mutation.
// Picker reprepare declares reset; decision copies compatible scalar state
// allocation-free. Deep cold clones own independent picker/filter histories.
signal_contracts::Status make_event_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
