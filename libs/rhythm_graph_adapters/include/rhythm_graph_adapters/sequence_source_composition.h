// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Ten source stages; all inputs are explicit Float32 data, output axes indices.
// NormalizedFeatureFusion/OnsetEvents: feature×time matrix0, weights-vector1.
// Fusion publishes fused0/maxima1. Events publishes final UInt8 mask0/times1/
// fused2/smoothed3/gated4/thresholds5/raw UInt8 decisions6. AdaptiveOnsetSelection
// takes curve0 and cumulative-weight scalar1; publishes mask0/times1/gated2/
// thresholds3/raw-decisions4. Selection settings delay5,frame_rate44100/512,
// alpha.1,silence_threshold.02. `reset_each_call=true` is explicit.
// OnsetRate: one complete PCM vector0, method0standard/1streaming, fixedFs44100;
// rate0/duration1/times2/HFC3/complex4/fused5/smoothed6/gated7/thresholds8/raw-mask9.
// BeatIntervalSummary: tick-seconds0, confidence scalar1; BPM0/confidence1/
// intervals2/estimates3/histogram4[250]/peaks5[2,3]/count6 UInt64. maximum_bpm1000.
// PcmRhythmSummary: PCM0; BPM0/confidence1/ticks2/intervals3/estimates4/histogram5/
// peaks6. method1MultiFeature/0SingleComplex, minimum_bpm40/maximum_bpm208.
// Missing source histogram estimates are unavailable independently.
// ActivationPeakSequence: activation-vector0; event-seconds0/frame-counter1 Int64/
// buffer-size2 UInt64. Seconds settings frame_rate100,threshold.5,smooth0,
// pre_average0/post_average0/pre_maximum0/post_maximum0,combine.03,delay0,onlinefalse.
// SpectralDifferenceSequence: time×feature matrix0; differences0/current1/count2
// UInt64. lag1/frequency_maximum1/positivefalse. First-chunk-size behavior is
// preserved with reset_each_call=false. PcmSpectralDifference: PCM0, same two
// matrices then last complex2/magnitude3/powered4/linear5/features6/window7/
// filter-center-Hz values8. Defaults retain 24 log-bands/octave and log10(1+x);
// frame_samples2048/fft_size0/hop_samples441/sample_rate44100/frame_rate0/window0
// (Hann),periodicfalse,automatic_lagtrue,overlap_ratio.5; lag1/max3/positivetrue.
// TrajectoryOnset: PCM0; events0/novelty1/band-history2/last-complex3/magnitude4/
// center-Hz-values5/local-first-sample6 Int64/frame-index7/acquisition-size8 UInt64/
// last FFT-input9/window10. frame_size2048/audio_hop256/novelty_chunk_size4096,
// maximum_onsets4096,sample_rate44100,band_sample_rate44100,threshold.05,
// ratio_threshold16,combine_ms20. Source filter/chunk reset quirks are retained.
// All buffers bounded before processing; callbacks synchronously write executor
// outputs, never retained. Full-input batches use provenance-only stamps. Empty
// chunks do not imply EOF. Raw/window/filter source math is not recalibrated.
signal_contracts::Status make_sequence_source_node(domain::NodeId,std::string_view,
    std::span<const domain::OwnedProcessingContract>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,domain::ConfiguredProcessingNode&);
}
