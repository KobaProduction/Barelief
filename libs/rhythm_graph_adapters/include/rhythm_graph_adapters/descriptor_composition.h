// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>

namespace rhythm_graph_adapters {
// Explicit source descriptor/filter stages. Contracts determine storage
// geometry only; sample rate, method, warp and coefficient edges are settings.
// Outputs carry the magnitude frame clock and index axes. In the band stage,
// center frequencies are separate numeric values, not mislabeled bin indices.
// Stable kinds and settings:
// OnsetDescriptor: UInt64 method=1, range0..9: energy/HFC/complex/phase/
// weighted-phase/MKL/KL/positive-flux/histogram-spectral-difference/old-HFC.
// Only methods2/3/4 require phase1. Scalar0 is the descriptor; histogram
// methods3/4/8 additionally publish weighted-counts1[10], rescaled-values2[N].
// SpectralOnset: UInt64 method=0, range0..5: HFC/complex/complex-phase/flux/
// Mel-flux/RMS; Float32 sample_rate=44100 (>0; Mel >=8000). Only1/2 need
// phase1. Scalar0 plus distances1[N] for1/2 or source Mel amp2db1[40] for4.
// TriangularSpectralBands: required Float32 edges_hz array, sample_rate=44100,
// UInt64 weighting=0 (linear/natural-log-Mel/log10-Mel), normalization=0
// (unit-sum/unit-triangle/unit-maximum), Bool square_magnitude/logarithm=true.
// Outputs: bands0[B], center-Hz-values1[B], weights2[B,N]; all payload axes
// are indices. Edges/coefficients are owned cold settings, never frame borrows.
// Optional scalar UInt8 control65500 uses the shared explicit event protocol.
// All values are finite; pair inputs have equal frame/session/coordinate grids.
// Histories persist inside an instance; reprepare explicitly declares reset,
// cold clone is deep. No FFT/framing/whitening/decision is hidden here.
signal_contracts::Status make_descriptor_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
