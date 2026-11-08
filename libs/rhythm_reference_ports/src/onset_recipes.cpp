// SPDX-License-Identifier: GPL-3.0-or-later
// Source parameter recipes from Aubio specdesc.c/onset.c at
// ad5cf975aed08cc4562dd008cf9f83b12b82ffb8, Copyright (C) 2003-2013 Paul Brossier;
// BTrack OnsetDetectionFunction.cpp at 9d6127618a5679e9caa74c594b88f1d74f0e035f,
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/onset_recipes.h>
#include <cmath>
#include <limits>
namespace rhythm_reference_ports {
namespace {
using dsp_kernels::SpectralDescriptor;
using dsp_kernels::SpectralDescriptorConfig;
bool histogram_recipe(OnsetRecipe method,OnsetDescriptorRecipe& result) noexcept {
    if(method==OnsetRecipe::Phase)result.histogram.method=HistogramOnsetMethod::Phase;
    else if(method==OnsetRecipe::WeightedPhase)result.histogram.method=HistogramOnsetMethod::WeightedPhase;
    else if(method==OnsetRecipe::SpectralDifference)result.histogram.method=HistogramOnsetMethod::SpectralDifference;
    else return false;
    result.kind=OnsetDescriptorKind::Histogram;return true;
}
bool basic_descriptor(OnsetRecipe method,SpectralDescriptorConfig& c) noexcept {
    if(method==OnsetRecipe::Energy)c.method=SpectralDescriptor::SpectralEnergy;
    else if(method==OnsetRecipe::FrequencyContent || method==OnsetRecipe::OldDefault)c.method=SpectralDescriptor::HighFrequencyContent;
    else if(method==OnsetRecipe::Complex)c.method=SpectralDescriptor::ComplexDifference;
    else if(method==OnsetRecipe::SpectralFlux){c.method=SpectralDescriptor::Flux;c.half_wave=true;}
    else return false;
    return true;
}
bool spectral_recipe(OnsetRecipe method,SpectralDescriptorConfig& c) noexcept {
    c.aggregation=dsp_kernels::SpectralAggregation::Sum;c.half_wave=false;
    if(basic_descriptor(method,c))return true;
    if(method!=OnsetRecipe::KullbackLeibler && method!=OnsetRecipe::ModifiedKullbackLeibler)return false;
    c.method=method==OnsetRecipe::KullbackLeibler?SpectralDescriptor::KullbackLeibler:SpectralDescriptor::ModifiedKullbackLeibler;
    c.epsilon=.1f;c.divergence_arithmetic=dsp_kernels::SpectralDivergenceArithmetic::Float64RatioFloat32Log;
    c.ratio_epsilon=.1;return true;
}



}
bool onset_descriptor_recipe(OnsetRecipe method,OnsetDescriptorRecipe& output) noexcept {
    OnsetDescriptorRecipe result;result.spectral.compensated_sum=false;result.spectral.stable_log1p=false;
    result.spectral.complex_distance=dsp_kernels::ComplexDistanceMode::SquaredCosine;
    if(!histogram_recipe(method,result) && !spectral_recipe(method,result.spectral))return false;
    output=result;return true;
}

}
