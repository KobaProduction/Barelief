// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/spectral_onset.h>
#include <dsp_kernels/prepared_storage.h>
#include <array>
#include <cmath>

namespace rhythm_reference_ports {
namespace {
bool mel_storage(std::size_t bins,float rate,std::size_t& bytes) noexcept {
    std::array<float,42> edges{};TriangularSpectralBandConfig config;config.sample_rate=rate;
    config.weighting=TriangleWarp::Log10Mel;config.logarithm=false;std::size_t descriptor=0;
    return rate>=8000 && warped_band_edges(edges,0,4000,TriangleWarp::Log10Mel) &&
        PreparedTriangularSpectralBands::storage_bytes(bins,edges,config,bytes) &&
        dsp_kernels::PreparedSpectralDescriptor::storage_bytes(40,descriptor) &&
        dsp_kernels::prepared_storage::add_bytes(bytes,descriptor) &&
        dsp_kernels::prepared_storage::add_array(bytes,40,sizeof(float));
}
}
bool PreparedSpectralOnset::storage_bytes(std::size_t bins,SpectralOnsetConfig config,std::size_t& output) noexcept {
    if(!bins || !std::isfinite(config.sample_rate) || config.sample_rate<=0 ||
       unsigned(config.method)>unsigned(SpectralOnsetMethod::Rms))return false;
    std::size_t bytes=0;
    if(config.method==SpectralOnsetMethod::MelFlux){if(!mel_storage(bins,config.sample_rate,bytes))return false;}
    else if(config.method==SpectralOnsetMethod::Complex || config.method==SpectralOnsetMethod::ComplexPhase){
        const auto method=config.method==SpectralOnsetMethod::Complex?PolarNovelty::Complex:PolarNovelty::ComplexPhase;
        if(!PreparedPolarNovelty::storage_bytes(bins,method,bytes) ||
           !dsp_kernels::prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    }
    else if(config.method!=SpectralOnsetMethod::Rms && !dsp_kernels::PreparedSpectralDescriptor::storage_bytes(bins,bytes))return false;
    output=bytes;return true;
}
std::size_t PreparedSpectralOnset::workspace_bytes() const noexcept {
    return descriptor_.workspace_bytes()+polar_.workspace_bytes()+mel_.workspace_bytes()+scratch_.capacity()*sizeof(float);
}
}
