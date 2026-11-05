// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/trajectory_onset_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <cmath>

namespace rhythm_reference_ports {
namespace {
dsp_kernels::TemporalWindowConfig trajectory_storage_window(TrajectoryOnsetRecipeConfig c) noexcept {
    dsp_kernels::TemporalWindowConfig window;window.length=c.frame_size;window.hop=c.audio_hop;window.periodic=false;
    window.center=true;window.odd_center_ceil=true;window.tail=dsp_kernels::TemporalTail::CenterCoverEnd;return window;
}
ContrastPeakPickerConfig trajectory_storage_peaks(TrajectoryOnsetRecipeConfig c) noexcept {
    ContrastPeakPickerConfig peak;peak.frame_rate=c.sample_rate/float(c.audio_hop);peak.threshold=c.threshold;
    peak.ratio_threshold=c.ratio_threshold;peak.combine_ms=c.combine_ms;return peak;
}
}
namespace {
TriangularSpectralBandConfig trajectory_storage_bands(TrajectoryOnsetRecipeConfig c) noexcept {
    TriangularSpectralBandConfig band;band.sample_rate=c.band_sample_rate;band.logarithm=false;return band;
}
}

bool PreparedTrajectoryOnsetRecipe::storage_bytes(std::size_t maximum,std::size_t capacity,TrajectoryOnsetRecipeConfig c,std::size_t& output,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(c.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    if(!maximum || !capacity || c.frame_size<2 || !c.audio_hop || !c.novelty_chunk_size || !std::isfinite(c.sample_rate) || c.sample_rate<=0)return false;
    const auto window=trajectory_storage_window(c);
    std::size_t frames=0,bytes=0,nested=0;const auto bins=c.frame_size/2+1,bands=trajectory_band_edges().size()-2;
    const auto band=trajectory_storage_bands(c);
    const auto peak=trajectory_storage_peaks(c);
    using namespace dsp_kernels::prepared_storage;
    if(!dsp_kernels::PreparedTemporalWindow::frame_count(window,maximum,frames) || !frames ||
       !dsp_kernels::PreparedFourierTempogram::storage_bytes(window,{},c.frame_size,true,bytes,factory) ||
       !PreparedTriangularSpectralBands::storage_bytes(bins,trajectory_band_edges(),band,nested) || !add_bytes(bytes,nested) ||
       !PreparedBufferedBandFlux::storage_bytes(bands,3,{8,2},nested) || !add_bytes(bytes,nested) ||
       !PreparedContrastPeakAccumulator::storage_bytes(c.novelty_chunk_size,capacity,peak,nested) || !add_bytes(bytes,nested))return false;
    if(!add_array(bytes,bands,sizeof(ScalarHistory)+8*sizeof(float)) || !add_array(bytes,bins,3*sizeof(float)) ||
       !add_array(bytes,c.frame_size,sizeof(float)) || !add_array(bytes,c.novelty_chunk_size,sizeof(float)) || !add_array(bytes,capacity,sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedTrajectoryOnsetRecipe::workspace_bytes() const noexcept {
    std::size_t bytes=spectrum_.workspace_bytes()+bands_.workspace_bytes()+novelty_.workspace_bytes()+peaks_.workspace_bytes()+history_.capacity()*sizeof(ScalarHistory);
    for(const auto& history:history_)bytes+=history.workspace_bytes();
    return bytes+(complex_.capacity()+magnitude_.capacity()+band_values_.capacity()+delayed_.capacity()+chunk_.capacity()+onsets_.capacity())*sizeof(float);
}
}
