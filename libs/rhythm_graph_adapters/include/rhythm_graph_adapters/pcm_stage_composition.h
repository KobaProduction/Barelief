// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// HopSpectrum/PcmOnsetSequence/PcmContextTempo take one fixed native Float32
// PCM hop0. Settings: UInt64 sample_rate=44100, window_samples=1024,
// hop_samples=512, window_shape=3 (source analysis enum0..8); method0..9,
// default1 for onset and7 for tempo. HopSpectrum accepts Float32 coefficients.
// ContextBeatTracker takes one fixed tracker_window=512 Float32 ODF vector0;
// ContextBeatSequence takes thresholded descriptor0 Float32 and silence1 UInt8.
// Tracker settings: frame_rate=44100/512, preferred_bpm=120,
// context_variance=3.901, min_period_frames=25; sequence tracker_window=0 uses
// its source resolver. Sequence hop_samples=512, Int32 delay_samples=0,
// tatums_per_beat=4. PCM tempo derives frame_rate from its own explicit Fs/hop,
// and adds threshold=.3/silence_db=-90; no frame metadata supplies DSP settings.
//
// Outputs (all Float32 unless specified):
// HopSpectrum: magnitude0, phase1, complex2[bin,ReIm], raw-history3,
// coefficients4, windowed5, FFT-input6, first-local-sample7 Int64.
// Onset: descriptor0, peak-position1, thresholded2, mean3, statistic4,
// detected5 UInt8, onset-sample6 UInt64, silent7 UInt8, processed-magnitude8,
// phase9, whitening-peaks10, complex11, raw12, coefficients13, windowed14,
// FFT-input15, first-local-sample16 Int64.
// Tempo: descriptor0/peak1/thresholded2, scheduled3 UInt8, tactus4,
// period-samples5/BPM6/confidence7, beat-sample8/tatum-sample9 Int64,
// poll10 UInt32, magnitude11/phase12, ODF-history13/ACF14/comb15/phase-response16,
// schedule17, complex18/raw19/coefficients20/windowed21/FFT-input22/origin23 Int64.
// Tracker: offsets0, period-frames1/BPM2/confidence3, meter4 UInt32,
// ACF5/comb6/phase-response7. Sequence: scheduled0/silent1 UInt8, tactus2,
// period-samples3/BPM4/confidence5, beat6/tatum7 Int64, poll8 UInt32,
// ODF-history9/ACF10/comb11/phase-response12/offsets13.
//
// FFT rows have explicit Float64LE Hz coordinates from own Fs/N. Raw/windowed
// PCM coordinates are local acquisition seconds from the prepared/reset stream
// origin (including initial zero padding); they are not incoming timestamps.
// Other axes are indices. Source provenance/tick clock is retained without
// fabricating an incoming hop interval for the larger source window.
// Warm HeldValue controls: threshold10/silence11 Float32 (PCM recipes),
// compression12 Float32/whiten13 UInt8 (onset), delay14 UInt64(onset) or Int32
// (tempo/sequence), minimum15 UInt64(onset) or signature15 UInt32(tempo/sequence),
// profile16 UInt8(onset) or explicit tatum-poll16 UInt8(tempo/sequence).
// HopSpectrum has shape17 UInt32 and coefficients18 Float32[N]. Profile applies
// before explicit overrides. Protocol65500 is optional UInt8; EOF without a
// complete frame emits unavailable outputs and consumes no synthetic tail.
// Source reset: onset clocks only; spectrum/context restart full analysis.
// Cold clones are independent; reprepare explicitly declares reset. Arithmetic
// failure preserves the source consumed prefix. Confidence/comb/phase-response
// keep source IEEE edges explicitly; other values and all axes are finite.
signal_contracts::Status make_pcm_stage_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
