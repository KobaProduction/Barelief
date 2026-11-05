#include "essentia_family_export.h"
#include <rhythm_reference_ports/beat_interval_histogram.h>
#include <rhythm_reference_ports/tempo_change_sections.h>
#include <rhythm_reference_ports/harmonic_tempo.h>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
void ibi(const Case& c,native_export::Output& out) {
    r::BeatIntervalHistogram result;native_export::require(r::beat_interval_histogram(c.input,result),"IBI histogram");
    out.row<float>("histogram.f32",result.histogram);
    for(std::size_t i=0;i<2;++i){const auto& p=result.peaks[i];out.values<float>("peaks.f32",{p.bpm,p.weight,p.spread});}
}
void rubato(const Case& c,native_export::Output& out) {
    r::PreparedTempoChangeSections operation;native_export::require(operation.prepare(c.input.size()),"rubato prepare");
    native_export::require(operation.process(c.input),"rubato process");
    out.row<float>("starts.f32",operation.starts());out.row<float>("stops.f32",operation.stops());
    out.values<std::uint64_t>("count.u64",{operation.section_count()});
}
void harmonic(const Case& c,native_export::Output& out) {
    r::PreparedHarmonicTempoCandidates operation;native_export::require(operation.prepare(c.input.size()),"harmonic prepare");
    native_export::require(operation.process(c.input),"harmonic process");out.row<float>("harmonic.f32",operation.values());
}
}
void intervals(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="ibi")ibi(c,out);else if(method=="rubato")rubato(c,out);else harmonic(c,out);
}
}
