// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Five independent source stages. PcmPredictiveOnset/PcmPredictiveBeatTracker
// take one complete Float32 hop0, CombTempoTransition a512-vector0,
// BeatPrediction/PredictiveBeatTracker scalar ODF0. Defaults: hop_samples512,
// frame_samples1024, method6 (10 source ODFs0..9), window_shape1 (0..4),
// history_size512/initial_period43/tightness5/alpha.9 for standalone prediction.
// Source Fs=44100; raw/full FFT magnitudes are never calibrated substitutes.
// Frontend outputs ODF0/full-complex1[N,2]/magnitude2/phase3/raw4/window5.
// Energy methods leave FFT-derived outputs unavailable, not stale.
// Comb outputs period0/BPM1/comb2[128]/observations3[41]/probabilities4[41].
// Prediction outputs beat0 UInt8/next-beat1 and next-prediction2 Int64/period3/
// scores4. Tracker adds BPM4/ODF-history5/scores6/resampled7[512]/comb8[128]/
// observations9/probabilities10. PCM tracker prefixes onset0 then tracker1..11
// and frontend complex12/magnitude13/phase14/raw15/window16. Payload axes are
// indices in original full-spectrum source order. Every input/output is finite.
// The factory must explicitly supply a SincBestQuality whole-window provider
// for tracker stages, including ratio1; absent provider is unsupported. Prepared
// processors/clones pin the exact provider and independently rebind callbacks.
// Optional held scalar controls: 10 UInt32 method (frontend/PCM tracker),
// 11 Float32 tempo and 12 UInt8 tempo-fixed (comb/tracker/PCM tracker),
// 13 Float32 period and 14 Float32 indicated-period (standalone prediction).
// Fixed=0 releases; fixed=1 fixes tempo11, or the current BPM when absent.
// Held values apply on change, before the next frame, without reseeding on
// every unchanged frame. Optional UInt8 protocol65500 carries explicit
// continue/batch/EOF/reset. Hop/frame/window geometry remains cold preparation;
// allocating source resize APIs never run on the processing path.
signal_contracts::Status make_predictive_operation_node(domain::NodeId,std::string_view,
    const domain::OwnedProcessingContract& input,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,domain::ConfiguredProcessingNode&);
signal_contracts::Status make_predictive_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,domain::ConfiguredProcessingNode&);
}
