#pragma once
#include <domain/processing_contracts.h>

namespace dsp_graph_adapters {
// SpectralNovelty accepts feature matrix0 [time,feature], and phase matrix1
// only for methods 4,6..10. Method0..11 follows SpectralNoveltyMethod; default2.
// Phase supports are explicit support_first/support_count arrays (one per
// feature). Output0 is [time,1], or [time,feature] for aligned difference11;
// outputs1..4 are last differences, phase mask, alignment correlations/offset.
// All operations are independent batches. Rates come from explicit settings;
// output axes default to indices. The helper does no DSP and owns all metadata.
signal_contracts::Status make_spectral_operation_node(domain::NodeId,std::string_view,
    std::span<const domain::ProcessingPort>,domain::TransformProcessingParameters,
    const signal_contracts::FrameLimits&,std::size_t maximum_owned_metadata_bytes,
    domain::ConfiguredProcessingNode&);
}
