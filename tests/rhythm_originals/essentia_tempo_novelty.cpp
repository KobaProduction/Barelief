#include "source_fourier_profile.h"
#include "essentia_tempo_export.h"
#include <rhythm_reference_ports/novelty_tempo_histogram.h>
namespace essentia_tempo_export {
namespace {
void peaks(native_export::Output& out,std::span<const rhythm_reference_ports::InterpolatedPeak> values,
           const char* positions,const char* magnitudes) {
    for(const auto& peak:values){out.values<float>(positions,{peak.position});out.values<float>(magnitudes,{peak.value});}
    if(values.empty()){out.row<float>(positions,{});out.row<float>(magnitudes,{});}
}
}
void novelty(const Case& c) {
    rhythm_reference_ports::PreparedNoveltyTempoHistogram recipe;
    rhythm_reference_ports::NoveltyTempoHistogramConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;config.frame_rate=float(c.data.rate)/float(c.data.hop);
    native_export::require(recipe.prepare(c.data.pcm.size(),config,{},native_export::source_fourier_backend()) && recipe.process(c.data.pcm),"novelty histogram recipe");
    const auto& result=recipe.result();native_export::Output out(c.data.prefix);
    out.values<float>("bpm.f32",{result.bpm()});out.row<float>("frame_bpms.f32",result.frame_bpms());
    out.row<float>("sinusoid.f32",result.pulse());peaks(out,result.candidates(),"candidates.f32","magnitudes.f32");
    peaks(out,result.ticks(),"ticks.f32","tick_magnitudes.f32");
    out.row<float>("tempogram.f32",{});
    for(std::size_t i=0;i<result.frames();++i)out.row<float>("tempogram.f32",result.tempogram_frame(i));
    out.values<std::uint64_t>("tempogram_shape.u64",{result.frames(),result.frames()?result.tempogram_frame(0).size():0});
}
}
