#pragma once
#include "operation_support.h"
#include <dsp_kernels/spectral_novelty.h>

namespace dsp_graph_adapters::detail {
struct SpectralSequencePlan {
    dsp_kernels::SpectralNoveltyConfig novelty;
    std::size_t frames=0,features=0,bins=0,columns=0,kernel_bytes=0;
};
struct SpectralOutputShape {
    signal_contracts::Rank rank=signal_contracts::Rank::Vector;
    std::size_t rows=1,columns=1;
    signal_contracts::DType dtype=signal_contracts::DType::Float32;
};
bool spectral_sequence_plan(const dataflow_runtime::NumericOperationNodeView&,SpectralSequencePlan&) noexcept;
bool spectral_supports(const dataflow_runtime::NumericOperationNodeView&,std::size_t features,std::size_t bins) noexcept;
std::vector<dsp_kernels::FilterBandSupport> own_spectral_supports(const dataflow_runtime::NumericOperationNodeView&);
SpectralOutputShape spectral_output_shape(const SpectralSequencePlan&,domain::PortId) noexcept;
bool spectral_sequence_outputs(const dataflow_runtime::NumericOperationNodeView&,const SpectralSequencePlan&) noexcept;
}
