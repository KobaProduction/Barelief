// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
#include <rhythm_graph_adapters/stage_ports.h>

namespace rhythm_graph_adapters {
// Explicit context scheduling. Port names and stable
// wire IDs are declared in stage_ports.h. All settings are independent from
// input metadata. ContextBeatTracker accepts fixed Float32 reversed-weighted
// ODF input0 and unbiased ACF input1, with tracker_window=512.
// ContextBeatSequence accepts previous ODF history0, UInt8 silence1,
// reversed-weighted ODF2 and unbiased ACF3 from the same frame/session.
// History, array arithmetic, reindexing and correlation are explicit graph nodes.
// Tracker settings: frame_rate=44100/512, preferred_bpm=120,
// context_variance=3.901, min_period_frames=25. A sequence tracker_window=0
// uses its source resolver. Sequence hop_samples=512, delay_samples=0,
// tatums_per_beat=4 and explicit poll_tatum select sample-clock behavior.
// Outputs expose scheduling, period, BPM, confidence and response curves.
// Sequence output9 forwards the explicitly connected previous history.
// Histories and response curves use index axes. Sample outputs are local
// Int64 counters; tatum event is UInt32 (0 none, 1 subdivision, 2 beat).
// Sequence HeldValue controls are delay14 Int32, signature15 UInt32 and
// poll16 UInt8. Protocol65500 is optional UInt8. EOF without a complete
// input group consumes no synthetic tail. Cold clones own their state.
// Reprepare resets context; arithmetic failure preserves the consumed prefix.
// Confidence/comb/phase-response preserve source IEEE edge cases explicitly.
signal_contracts::Status make_pcm_stage_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
