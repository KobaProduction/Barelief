#pragma once
#include <domain/processing_contracts.h>

namespace dsp_graph_adapters {
// SpectralNovelty accepts feature matrix0 [time,feature], and phase matrix1
// only for methods 4,6..10. Method0..11 follows SpectralNoveltyMethod; default2.
// Phase supports are explicit support_first/support_count arrays (one per
// feature). Output0 is [time,1], or [time,feature] for aligned difference11;
// outputs1..4 are last differences, phase mask, alignment correlations/offset.
// PcmSpectralNovelty accepts PCM0. Its explicit framing/window/filter/log
// settings are declared by spectral_sequence_parameters; raw Fourier source
// conventions are preserved. Output0 as above;1 complex[bin,2];2 raw magnitude;
// 3 phase;4 features;5..8 novelty diagnostics. Uncomputed last-stage views are
// empty, not stale. Calibration is never inferred from input metadata.
// ActivationPeakMatrix accepts activations0 [time,channel], independent time
// smoothing/picking per column. Outputs0/1 are UInt64 frame/channel indices,
// sorted frame then channel. Smoothing uses source Input crop even for one
// channel; source-valid empty results have actual extent0.
// All operations are independent batches. Rates come from explicit settings;
// output axes default to indices. The helper does no DSP and owns all metadata.
signal_contracts::Status make_spectral_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::ProcessingPort>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode&);
}
