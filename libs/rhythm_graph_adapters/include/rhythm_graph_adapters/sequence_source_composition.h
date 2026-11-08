// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Six source stages; all inputs are explicit Float32 data, output axes indices.
// NormalizedFeatureFusion/OnsetEvents: feature x time matrix0, weights-vector1.
// Fusion publishes fused0/maxima1. Events publishes final UInt8 mask0/times1/
// fused2/smoothed3/gated4/thresholds5/raw UInt8 decisions6. AdaptiveOnsetSelection
// takes curve0 and cumulative-weight scalar1; publishes mask0/times1/gated2/
// thresholds3/raw-decisions4. Selection settings delay5,frame_rate44100/512,
// alpha.1,silence_threshold.02. `reset_each_call=true` is explicit.
// BeatIntervalSummary: tick-seconds0, confidence scalar1; BPM0/confidence1/
// intervals2/estimates3/histogram4[250]/peaks5[2,3]/count6 UInt64. maximum_bpm1000.
// Missing source histogram estimates are unavailable independently.
// ActivationPeakSequence: activation-vector0; event-seconds0/frame-counter1 Int64/
// buffer-size2 UInt64. Seconds settings frame_rate100,threshold.5,smooth0,
// pre_average0/post_average0/pre_maximum0/post_maximum0,combine.03,delay0,onlinefalse.
// SpectralDifferenceSequence: time x feature matrix0; differences0/current1/count2
// UInt64. lag1/frequency_maximum1/positivefalse.
// First-chunk-size behavior is preserved with reset_each_call=false.
// All buffers bounded before processing; callbacks synchronously write executor
// outputs, never retained. Full-input batches use provenance-only stamps. Empty
// chunks do not imply EOF. Raw/window/filter source math is not recalibrated.
signal_contracts::Status make_sequence_source_node(domain::NodeId,std::string_view,
    std::span<const domain::OwnedProcessingContract>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,domain::ConfiguredProcessingNode&);
}
