// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Independent complete-batch stages, never a second executor or PCM frontend.
// EssentiaOnsetRows: magnitude0/phase1 [frame,1025], method1/4/5
// (complex/MelFlux/RMS); curve0, Float64 sample times1, intermediate rows2
// when applicable. Explicit frame_rate=44100/1024 (not transport metadata).
// EssentiaInformationRows: magnitude0 [frame,1025] -> curve0, Float64 times1;
// bins2..232, explicit frame_rate=44100/512. Times are output ODF-grid seconds;
// the source information value retains its five-frame retrospective delay.
// EssentiaNoveltyRows: magnitude0/phase1 -> source PLUS-phase distances0.
// EssentiaErbRows: distances0 -> ERB rows0, center-Hz values1, weights2.
// EssentiaAgreement: five candidate tick vectors0..4 -> ticks0, confidence1,
// selected UInt64 method2, scores3, pairwise information gain4.
// EssentiaCandidateInputs: five ODF vectors0..4 -> unchanged vectors0..4;
// a current-session barrier preserves shared RNG draw order in five separate
// downstream trackers. Each vector has an independent capacity, no kernel state.
// All analytical values Float32, axes indices, fixed source Fs44100/N2048.
// Known PCM Fs and frequency grids must match that source declaration. Explicit
// Hz/index coordinate payloads are validated against k*44100/2048 or k before
// resetting any row state; unknown/index axes never select a different bank.
// Every batch resets row histories; variable row counts retain source EOF.
signal_contracts::Status make_essentia_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::OwnedProcessingContract>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,
    domain::ConfiguredProcessingNode&);
}
