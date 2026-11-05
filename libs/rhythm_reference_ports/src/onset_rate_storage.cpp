// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/onset_rate_recipe.h>
#include <dsp_kernels/prepared_storage.h>

namespace rhythm_reference_ports {
bool PreparedOnsetRateRecipe::storage_bytes(std::size_t maximum,OnsetRateRecipe recipe,std::size_t& output,dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    if(!maximum || maximum>INT32_MAX || unsigned(recipe)>unsigned(OnsetRateRecipe::Streaming))return false;
    dsp_kernels::TemporalWindowConfig window;window.length=1024;window.hop=512;window.periodic=false;
    window.center=recipe==OnsetRateRecipe::Streaming;window.odd_center_ceil=true;
    window.tail=window.center?dsp_kernels::TemporalTail::CenterCoverEnd:dsp_kernels::TemporalTail::CoverEnd;
    std::size_t frames=0,bytes=0,nested=0;using namespace dsp_kernels::prepared_storage;
    if(!dsp_kernels::PreparedTemporalWindow::frame_count(window,maximum,frames) ||
       !dsp_kernels::PreparedFourierTempogram::storage_bytes(window,{},1024,true,bytes,factory) ||
       !dsp_kernels::PreparedSpectralDescriptor::storage_bytes(513,nested) || !add_bytes(bytes,nested) ||
       !PreparedPolarNovelty::storage_bytes(513,PolarNovelty::Complex,nested) || !add_bytes(bytes,nested) ||
       !PreparedOnsetEvents::storage_bytes(2,frames,{},nested) || !add_bytes(bytes,nested) ||
       !add_array(bytes,513,5*sizeof(float)) || !add_array(bytes,1024,sizeof(float)) ||
       !add_array(bytes,frames,3*sizeof(float)+sizeof(std::uint8_t)))return false;
    output=bytes;return true;
}
std::size_t PreparedOnsetRateRecipe::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+hfc_.workspace_bytes()+complex_novelty_.workspace_bytes()+events_.workspace_bytes()+decisions_.capacity()+
        (complex_.capacity()+magnitude_.capacity()+phase_.capacity()+distances_.capacity()+hfc_curve_.capacity()+complex_curve_.capacity()+times_.capacity())*sizeof(float);
}
}
