// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/processing_contracts.h>
namespace rhythm_graph_adapters {
// Explicit primitive/whole-recipe builders. Inputs are in port order, Float32
// native mono vectors, except PeriodPath/BeatAgreement/HarmonicComb matrix input0
// matrices and BeatAgreement input1 UInt64 per-row counts. Public output tables
// and source settings are documented in tracking_operations.md. No implicit
// PCM/spectrum conversion, source clock inference, or hidden history is added.
// The optional rhythm factory must be installed in NumericRuntimeProfile.
// Cold composition owns contracts/parameters; it performs no source processing.
// FrameSilencePolicy additionally permits optional scalar UInt8 protocol65500
// in the explicit-port overload. EOF finalizes without acquiring another frame;
// packet size never implies EOF. Control ports follow the ordered data ports.
signal_contracts::Status make_tracking_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::OwnedProcessingContract> inputs,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,
    domain::ConfiguredProcessingNode& output);
signal_contracts::Status make_tracking_operation_node(domain::NodeId,std::string_view kind,
    std::span<const domain::ProcessingPort> inputs,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata,
    domain::ConfiguredProcessingNode& output);
}
