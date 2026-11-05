#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/pcm_rhythm_summary.h>
#include <cstdlib>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
float noise(void*) noexcept {return float(std::rand())/float(RAND_MAX);}
void histogram(const r::PreparedPcmRhythmSummary& operation,native_export::Output& out) {
    r::BeatIntervalHistogram result;native_export::require(operation.histogram_descriptors(result),"summary histogram");
    out.row<float>("histogram.f32",result.histogram);
    for(const auto& peak:result.peaks)out.values<float>("peaks.f32",{peak.bpm,peak.weight,peak.spread});
}
}
void summary(const Case& c,const std::string& method) {
    r::PcmBeatRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;config.noise={nullptr,noise};
    if(method=="summary_single")config.method=r::PcmBeatMethod::SingleComplex;
    r::PreparedPcmRhythmSummary operation;native_export::require(operation.prepare(c.input.size(),config,native_export::source_fourier_backend()),"summary prepare");
    native_export::require(operation.process(c.input),"summary process");const auto& result=operation.summary();
    native_export::Output out(c.prefix);out.values<float>("bpm.f32",{result.bpm()});out.values<float>("confidence.f32",{result.confidence()});
    out.row<float>("ticks.f32",operation.tracker().ticks());out.row<float>("estimates.f32",result.estimates());
    // Standard RhythmDescriptors::compute leaves its public bpm_intervals
    // assignment commented out. Its inner IBI still feeds the histogram.
    out.row<float>("intervals.f32",method=="descriptors"?std::span<const float>{}:result.intervals());
    if(method=="descriptors")histogram(operation,out);
}
}
