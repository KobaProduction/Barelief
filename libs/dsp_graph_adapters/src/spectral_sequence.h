#pragma once
#include "operation_support.h"
#include <dsp_kernels/spectral_novelty.h>
#include <dsp_kernels/pcm_spectral_novelty.h>
#include <dsp_kernels/activation_peak_matrix.h>

namespace dsp_graph_adapters::detail {
struct SpectralSequencePlan {
    dsp_kernels::PcmSpectralNoveltyConfig pcm;
    dsp_kernels::ActivationPeakConfig peaks;
    dsp_kernels::ExplicitSpectralFilters filters;
    std::size_t samples=0,frames=0,features=0,bins=0,columns=0,kernel_bytes=0;
    bool pcm_input=false,matrix_peaks=false;
};
struct SpectralOutputShape {
    signal_contracts::Rank rank=signal_contracts::Rank::Vector;
    std::size_t rows=1,columns=1;
    signal_contracts::DType dtype=signal_contracts::DType::Float32;
};
bool spectral_sequence_plan(const domain::ResolvedProcessingNode&,SpectralSequencePlan&) noexcept;
bool spectral_supports(const domain::ResolvedProcessingNode&,std::size_t features,std::size_t bins) noexcept;
std::vector<dsp_kernels::FilterBandSupport> own_spectral_supports(const domain::ResolvedProcessingNode&);
SpectralOutputShape spectral_output_shape(const SpectralSequencePlan&,domain::PortId) noexcept;
bool spectral_sequence_outputs(const domain::ResolvedProcessingNode&,const SpectralSequencePlan&) noexcept;
}
