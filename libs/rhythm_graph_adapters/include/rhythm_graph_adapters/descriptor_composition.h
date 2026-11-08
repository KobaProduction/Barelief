// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
#include <rhythm_graph_adapters/stage_ports.h>

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
// positive-flux/RMS; Float32 sample_rate=44100 (>0). Only1/2 need phase1.
// Method4 accepts supplied signed log values; projection/log are upstream.
// Scalar0 plus distances1[N] for1/2 or supplied values1[N] for4.
signal_contracts::Status make_descriptor_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters parameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode& output);
}
