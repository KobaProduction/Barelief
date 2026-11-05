// SPDX-License-Identifier: GPL-3.0-or-later
// aubio specdesc.c recipes at ad5cf975aed08cc4562dd008cf9f83b12b82ffb8.
// Copyright (C) 2003-2009 Paul Brossier; C++ composition (C) 2026 Koba Production.
// GPL3 or later; no warranty, see ../COPYING.
#include <rhythm_reference_ports/onset_descriptor.h>
#include <utility>
namespace rhythm_reference_ports {
bool PreparedOnsetDescriptor::prepare(std::size_t bins,OnsetRecipe method) {
    PreparedOnsetDescriptor next;std::size_t bytes=0;
    if(!storage_bytes(bins,method,bytes) || !onset_descriptor_recipe(method,next.recipe_))return false;
    const bool ready=next.recipe_.kind==OnsetDescriptorKind::Histogram?
        next.histogram_.prepare(bins,next.recipe_.histogram):next.spectral_.prepare(bins,next.recipe_.spectral);
    if(!ready || next.workspace_bytes()>bytes)return false;
    next.prepared_=true;*this=std::move(next);return true;
}
bool PreparedOnsetDescriptor::storage_bytes(std::size_t bins,OnsetRecipe method,std::size_t& bytes) noexcept {
    OnsetDescriptorRecipe recipe;if(!onset_descriptor_recipe(method,recipe))return false;
    return recipe.kind==OnsetDescriptorKind::Histogram?
        PreparedHistogramOnset::storage_bytes(bins,recipe.histogram,bytes):dsp_kernels::PreparedSpectralDescriptor::storage_bytes(bins,bytes);
}
std::size_t PreparedOnsetDescriptor::workspace_bytes() const noexcept {
    return spectral_.workspace_bytes()+histogram_.workspace_bytes();
}
void PreparedOnsetDescriptor::restart() noexcept {spectral_.reset();histogram_.reset();}
bool PreparedOnsetDescriptor::process(std::span<const float> magnitude,std::span<const float> phase,float& value) noexcept {
    if(!prepared_)return false;
    if(recipe_.kind==OnsetDescriptorKind::Histogram)return recipe_.histogram.method==HistogramOnsetMethod::SpectralDifference?
        histogram_.process_magnitude(magnitude,value):histogram_.process_polar(magnitude,phase,value);
    return recipe_.spectral.method==dsp_kernels::SpectralDescriptor::ComplexDifference?spectral_.process_polar(magnitude,phase,value):
        spectral_.process_values(magnitude,value);
}
}
