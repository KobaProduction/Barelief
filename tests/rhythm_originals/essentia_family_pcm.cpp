#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/activity_fluctuation_recipe.h>
#include <rhythm_reference_ports/beat_loudness_sequence.h>
#include <rhythm_reference_ports/legacy_rhythm_recipe.h>
#include <rhythm_reference_ports/loop_tempo_recipe.h>
#include <rhythm_reference_ports/onset_rate_recipe.h>
#include <rhythm_reference_ports/trajectory_onset_recipe.h>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
void dfa(const Case& c,native_export::Output& out) {
    r::PreparedActivityFluctuation operation;native_export::require(operation.prepare(c.input.size()),"DFA prepare");
    const bool success=operation.process(c.input);const auto& scaling=operation.scaling();
    out.values<float>("score.f32",{scaling.result().score});out.row<float>("dfa.f32",scaling.exponents());
    out.row<float>("integrated.f32",operation.integrated_activity());out.row<float>("fluctuations.f32",scaling.fluctuations());
    out.values<std::uint64_t>("status.u64",{std::uint64_t(scaling.result().status),std::uint64_t(success)});
}
void loudness(const Case& c,native_export::Output& out) {
    r::BeatLoudnessSequenceConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    r::PreparedBeatLoudnessSequence operation;native_export::require(operation.prepare(c.aux.size(),config,r::default_beats_loudness_borders,native_export::source_fourier_backend()),"loudness prepare");
    native_export::require(operation.process(c.input,c.aux),"loudness process");out.row<float>("loudness.f32",operation.energies());
    for(std::size_t i=0;i<operation.rows();++i)out.row<float>("ratios.f32",operation.band_ratios(i));
    out.values<std::uint64_t>("shape.u64",{operation.rows(),5});
}
void legacy(const Case& c,native_export::Output& out) {
    r::PreparedLegacyRhythmRecipe operation;const auto frames=c.input.size()/256+4;
    r::LegacyRhythmRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    native_export::require(operation.prepare({c.input.size(),frames*8,frames*32},config,{},native_export::source_fourier_backend()),"legacy prepare");
    native_export::require(operation.process(c.input),"legacy process");out.values<float>("bpm.f32",{operation.bpm()});
    out.row<float>("ticks.f32",operation.ticks());out.row<float>("estimates.f32",operation.bpm_estimates());out.row<float>("intervals.f32",operation.intervals());
}
void superflux(const Case& c,native_export::Output& out) {
    r::TrajectoryOnsetRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    r::PreparedTrajectoryOnsetRecipe operation;native_export::require(operation.prepare(c.input.size(),c.input.size()/256+4,config,native_export::source_fourier_backend()),"SuperFlux prepare");
    native_export::require(operation.process(c.input),"SuperFlux process");out.row<float>("onsets.f32",operation.onsets());
}
void loop(const Case& c,native_export::Output& out) {
    r::PreparedLoopTempoRecipe operation;native_export::require(operation.prepare(c.input.size(),.95f,dsp_kernels::NumericalPrecision::DesktopReference,native_export::source_fourier_backend()),"loop prepare");
    native_export::require(operation.process(c.input),"loop process");out.values<float>("bpm.f32",{operation.bpm()});
    out.values<float>("raw_bpm.f32",{operation.raw_bpm()});out.values<float>("confidence.f32",{operation.confidence().confidence});
}
void loop_confidence(const Case& c,native_export::Output& out) {
    r::PreparedLoopLengthConfidence operation;r::LoopLengthConfidenceResult result;
    native_export::require(c.aux.size()==1 && operation.prepare(float(c.rate)),"loop confidence prepare");
    native_export::require(operation.process(c.input,c.aux[0],result),"loop confidence process");out.values<float>("confidence.f32",{result.confidence});
}
void onsetrate(const Case& c,native_export::Output& out) {
    r::PreparedOnsetRateRecipe operation;native_export::require(operation.prepare(c.input.size(),r::OnsetRateRecipe::Standard,dsp_kernels::NumericalPrecision::DesktopReference,native_export::source_fourier_backend()),"onset rate prepare");
    native_export::require(operation.process(c.input),"onset rate process");out.values<float>("rate.f32",{operation.rate()});
    out.row<float>("onsets.f32",operation.onset_times());out.row<float>("hfc.f32",operation.hfc_curve());out.row<float>("complex.f32",operation.complex_curve());
}
}
namespace {
void pcm_rhythm(const Case& c,const std::string& method,native_export::Output& out) {
    if(method=="legacy")return legacy(c,out);
    if(method=="superflux")return superflux(c,out);
    if(method=="onsetrate")return onsetrate(c,out);
    throw std::runtime_error("unknown Essentia family");
}
}

void pcm(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="dfa")return dfa(c,out);
    if(method=="loudness")return loudness(c,out);
    if(method=="loop")return loop(c,out);
    if(method=="loop_confidence")return loop_confidence(c,out);
    pcm_rhythm(c,method,out);
}
}
