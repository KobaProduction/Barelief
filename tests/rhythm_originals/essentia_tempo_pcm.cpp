#include "source_fourier_profile.h"
#include "essentia_tempo_export.h"
#include <rhythm_reference_ports/pcm_beat_recipe.h>
namespace essentia_tempo_export {
void pcm_beat(const Case& c,bool multi) {
    native_export::require(c.data.rate==44100,"source beat PCM sample rate");
    rhythm_reference_ports::PreparedPcmBeatRecipe tracker;
    rhythm_reference_ports::PcmBeatRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    config.method=multi?rhythm_reference_ports::PcmBeatMethod::MultiFeature:rhythm_reference_ports::PcmBeatMethod::SingleComplex;
    config.noise={nullptr,noise};
    native_export::require(tracker.prepare(c.data.pcm.size(),config,native_export::source_fourier_backend()) && tracker.process(c.data.pcm),"PCM beat recipe");
    native_export::Output out(c.data.prefix);out.row<float>("ticks.f32",tracker.ticks());
    if(multi)out.values<float>("confidence.f32",{tracker.confidence()});
    for(std::size_t i=0;i<(multi?5u:1u);++i){
        out.row<float>("curve"+std::to_string(i)+".f32",tracker.onset_curve(i));
        out.row<float>("candidate"+std::to_string(i)+".f32",tracker.candidate_times(i));
    }
}
}
