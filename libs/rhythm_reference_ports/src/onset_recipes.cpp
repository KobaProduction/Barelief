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
bool predictive_difference(PredictiveOnsetMethod method,SpectralDescriptorConfig& c) noexcept {
    if(method==PredictiveOnsetMethod::SpectralDifference || method==PredictiveOnsetMethod::PositiveSpectralDifference){
        c.method=SpectralDescriptor::Flux;c.half_wave=method==PredictiveOnsetMethod::PositiveSpectralDifference;return true;
    }
    if(method==PredictiveOnsetMethod::ComplexDifference || method==PredictiveOnsetMethod::PositiveComplexDifference){
        c.method=SpectralDescriptor::ComplexDifference;c.half_wave=method==PredictiveOnsetMethod::PositiveComplexDifference;
        c.strict_half_wave=c.half_wave;return true;
    }
    if(method==PredictiveOnsetMethod::FrequencyDifference || method==PredictiveOnsetMethod::PositiveFrequencyDifference){
        c.method=SpectralDescriptor::HighFrequencyDifference;c.half_wave=method==PredictiveOnsetMethod::PositiveFrequencyDifference;return true;
    }
    return false;
}
bool predictive_spectral(PredictiveOnsetMethod method,SpectralDescriptorConfig& c) noexcept {
    if(predictive_difference(method,c))return true;
    if(method==PredictiveOnsetMethod::PhaseDeviation){c.method=SpectralDescriptor::PhaseDeviation;c.magnitude_threshold=.1f;return true;}
    if(method==PredictiveOnsetMethod::FrequencyContent){c.method=SpectralDescriptor::HighFrequencyContent;return true;}
    return false;
}
bool predictive_energy(PredictiveOnsetMethod method,PredictiveOnsetParameters& result) noexcept {
    if(method!=PredictiveOnsetMethod::EnergyEnvelope && method!=PredictiveOnsetMethod::EnergyDifference)return false;
    result.uses_pcm_energy=true;result.energy.positive_difference=method==PredictiveOnsetMethod::EnergyDifference;
    result.energy.compensated_sum=false;return true;
}
}
bool onset_descriptor_recipe(OnsetRecipe method,OnsetDescriptorRecipe& output) noexcept {
    OnsetDescriptorRecipe result;result.spectral.compensated_sum=false;result.spectral.stable_log1p=false;
    result.spectral.complex_distance=dsp_kernels::ComplexDistanceMode::SquaredCosine;
    if(!histogram_recipe(method,result) && !spectral_recipe(method,result.spectral))return false;
    output=result;return true;
}
bool onset_decision_parameters(OnsetRecipe method,std::uint32_t hop,float rate,OnsetDecisionConfig& output) noexcept {
    if(!hop || !std::isfinite(rate) || rate<=0)return false;
    OnsetDescriptorRecipe descriptor;if(!onset_descriptor_recipe(method,descriptor))return false;
    const auto recipe=onset_recipe_parameters(method);
    const float delay=recipe.delay_hops*float(hop),interval=std::round(recipe.minimum_interval_s*rate);
    if(!std::isfinite(delay) || delay>=float(UINT64_MAX) || !std::isfinite(interval) || interval>=float(UINT64_MAX))return false;
    output={hop,std::uint64_t(interval),std::uint64_t(delay)};return true;
}
bool predictive_onset_parameters(PredictiveOnsetMethod method,std::size_t fft,PredictiveOnsetParameters& output) noexcept {
    PredictiveOnsetParameters result;
    if(predictive_energy(method,result)){output=result;return true;}
    if(fft<2 || fft%2)return false;
    result.spectral.source_fft_size=fft;result.spectral.mirror_spectrum=true;
    result.spectral.aggregation=dsp_kernels::SpectralAggregation::Sum;result.spectral.compensated_sum=false;
    result.spectral.complex_distance=dsp_kernels::ComplexDistanceMode::SquaredCosineRaw;
    if(!predictive_spectral(method,result.spectral))return false;
    result.spectral.update_phase_history=method==PredictiveOnsetMethod::PhaseDeviation ||
        result.spectral.method==SpectralDescriptor::ComplexDifference;
    result.spectral.update_magnitude_history=method!=PredictiveOnsetMethod::PhaseDeviation;
    output=result;return true;
}
}
