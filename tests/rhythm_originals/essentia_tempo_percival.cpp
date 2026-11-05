#include "source_fourier_profile.h"
#include "essentia_tempo_export.h"
#include <rhythm_reference_ports/pulse_tempo_recipe.h>
namespace essentia_tempo_export {
void percival(const Case& c) {
    rhythm_reference_ports::PreparedPulseTempoRecipe recipe;
    rhythm_reference_ports::PulseTempoRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    config.sample_rate=int(c.data.rate);config.frame_size=c.data.frame;config.audio_hop=c.data.hop;
    native_export::require(recipe.prepare(c.data.pcm.size(),config,{},native_export::source_fourier_backend()) && recipe.process(c.data.pcm),"Percival recipe");
    native_export::Output out(c.data.prefix);out.values<float>("bpm.f32",{recipe.result().bpm});
    out.row<float>("last_magnitude.f32",recipe.last_magnitude());out.row<float>("last_compressed.f32",recipe.last_compressed());
    out.row<float>("last_correlation.f32",recipe.last_correlation());out.row<float>("last_enhanced.f32",recipe.last_enhanced());
    out.values<std::uint64_t>("lag_observations.u64",{recipe.lag_observations()});
    out.values<float>("uncorrected_bpm.f32",{recipe.result().uncorrected_bpm});
    out.values<float>("octave_multiplier.f32",{recipe.result().octave_multiplier});
    out.values<std::uint64_t>("selected_lag.u64",{recipe.result().lag});
}
}
