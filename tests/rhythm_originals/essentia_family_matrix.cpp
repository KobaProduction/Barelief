#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/correlation_meter.h>
#include <rhythm_reference_ports/beat_band_salience.h>
#include <rhythm_reference_ports/band_novelty.h>
#include <rhythm_reference_ports/fixed_tempo_candidates.h>
#include <rhythm_reference_ports/band_rhythm_transform.h>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
void meter(const Case& c,native_export::Output& out) {
    Matrix input{c.input,c.columns};r::PreparedCorrelationMeter operation;float result=0;
    native_export::require(operation.prepare(c.columns,c.rows,native_export::source_fourier_backend()),"meter prepare");
    native_export::require(operation.process(input.source(),c.rows,c.columns,result),"meter process");
    out.values<float>("meter.f32",{result});out.row<float>("correlation.f32",operation.summed_correlation());
}
void beatogram(const Case& c,native_export::Output& out) {
    Matrix input{c.input,c.columns};r::PreparedBeatBandSalience operation;
    native_export::require(operation.prepare(c.rows,c.columns),"beatogram prepare");
    native_export::require(operation.process(input.source(),c.aux,c.columns),"beatogram process");
    for(std::size_t b=0;b<operation.bands();++b)out.row<float>("beatogram.f32",operation.band_values(b));
    out.values<std::uint64_t>("shape.u64",{operation.bands(),operation.beats()});
}
void novelty(const Case& c,native_export::Output& out) {
    Matrix input{c.input,c.columns};r::PreparedBandNovelty operation;std::vector<float> result(c.rows-1);
    native_export::require(operation.prepare(c.rows,c.columns),"band novelty prepare");
    native_export::require(operation.process(input.source(),c.rows,result),"band novelty process");out.row<float>("novelty.f32",result);
}
void fixed(const Case& c,native_export::Output& out) {
    r::PreparedFixedTempoCandidates operation;native_export::require(operation.prepare(c.input.size(),{},native_export::source_fourier_backend()),"fixed BPM prepare");
    const bool success=operation.process(c.input);out.values<std::uint64_t>("status.u64",{std::uint64_t(operation.status()),std::uint64_t(success)});
    std::vector<float> positions,amplitudes;for(const auto& p:operation.candidates()){positions.push_back(p.position);amplitudes.push_back(p.value);}
    out.row<float>("bpms.f32",positions);out.row<float>("amplitudes.f32",amplitudes);out.row<float>("correlation.f32",operation.correlation());
}
bool rhythm_row(void* context,std::size_t,std::span<const float> values) noexcept {
    static_cast<native_export::Output*>(context)->row<float>("rhythm.f32",values);return true;
}
void rhythm_transform(const Case& c,native_export::Output& out) {
    Matrix input{c.input,c.columns};r::PreparedBandRhythmTransform operation;
    r::BandRhythmTransformConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
    native_export::require(operation.prepare(c.columns,config,native_export::source_fourier_backend()),"rhythm transform prepare");
    native_export::require(operation.process(input.source(),c.rows,{&out,rhythm_row}),"rhythm transform process");
    out.values<std::uint64_t>("shape.u64",{operation.frames(c.rows),operation.bins()});
}
}
void matrix(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="meter")meter(c,out);else if(method=="beatogram")beatogram(c,out);else if(method=="band_novelty")novelty(c,out);
    else if(method=="rhythm_transform")rhythm_transform(c,out);else fixed(c,out);
}
}
