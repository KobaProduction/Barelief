#include "source_fourier_profile.h"
#include "essentia_family_export.h"
#include <rhythm_reference_ports/spectral_onset.h>
#include <rhythm_reference_ports/pulse_train_analysis.h>
#include <rhythm_reference_ports/global_onset_recipe.h>
#include <array>
#include <limits>
namespace r=rhythm_reference_ports;
namespace essentia_family_export {
namespace {
struct PolarMethods {std::array<r::PreparedSpectralOnset,6> values;std::array<bool,6> available{};};
void polar_frame(PolarMethods& methods,std::span<const float> row,std::size_t bins,native_export::Output& out) {
    std::array<float,6> values;std::array<std::uint64_t,6> status{};
    for(std::size_t i=0;i<values.size();++i){
        values[i]=std::numeric_limits<float>::quiet_NaN();status[i]=std::uint64_t(!methods.available[i]);
        if(methods.available[i])native_export::require(methods.values[i].process(row.first(bins),row.subspan(bins),values[i]),"polar ODF process");
    }
    out.row<float>("odf.f32",values);out.row<std::uint64_t>("odf_status.u64",status);
}
void polar(const Case& c,native_export::Output& out) {
    native_export::require(c.input.size()==2*std::size_t(c.rows)*c.columns,"polar geometry");PolarMethods methods;
    for(std::size_t i=0;i<6;++i)methods.available[i]=methods.values[i].prepare(c.columns,{r::SpectralOnsetMethod(i),float(c.rate)});
    const auto input=std::span(c.input);
    for(std::size_t frame=0;frame<c.rows;++frame)polar_frame(methods,input.subspan(frame*2*c.columns,2*c.columns),c.columns,out);
}
void pulse(const Case& c,native_export::Output& out) {
    r::PreparedPulseTrainScoring operation;float lag=0;
    native_export::require(operation.prepare(c.input.size(),std::max(std::size_t{1},c.aux.size())),"pulse train prepare");
    native_export::require(operation.process(c.input,c.aux,lag),"pulse train process");out.values<float>("lag.f32",{lag});
    out.row<float>("magnitudes.f32",operation.magnitudes());out.row<float>("variances.f32",operation.variances());out.row<float>("scores.f32",operation.scores());
}
void weights(std::span<const float> values,native_export::Output& out) {
    std::vector<float> sorted(values.begin(),values.end());std::sort(sorted.begin(),sorted.end());
    const float threshold=sorted[std::size_t(float(values.size())*.6f)];std::vector<std::uint64_t> selected;
    for(std::size_t i=0;i<values.size();++i)if(values[i]>=threshold)selected.push_back(i);
    out.row<float>("weights.f32",values);out.values<float>("threshold.f32",{threshold});out.row<std::uint64_t>("selected.u64",selected);
}
void emphasis(const Case& c,native_export::Output& out) {
    native_export::require(c.aux.size()==1,"emphasis explicit frame rate");Matrix input{c.input,c.columns};
    r::PreparedBandBeatEmphasis operation;std::vector<float> values(c.rows);
    native_export::require(operation.prepare(c.rows,{c.aux[0],c.columns},native_export::source_fourier_backend()),"emphasis prepare");
    native_export::require(operation.process(input.source(),c.rows,values),"emphasis process");out.row<float>("curve.f32",values);weights(operation.band_weights(),out);
}
void write_global(const void* context,const r::GlobalOnsetFrameStages& stage) noexcept {
    auto& out=*const_cast<native_export::Output*>(static_cast<const native_export::Output*>(context));
    out.row<float>("erb.f32",stage.erb_values);out.row<float>("norm.f32",stage.spectrum.magnitude);out.row<float>("phase.f32",stage.spectrum.phase);
}
void global(const Case& c,native_export::Output& out) {
    r::GlobalOnsetRecipeConfig config;config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;config.sample_rate=float(c.rate);config.frame_size=c.rows;config.hop=c.columns;config.method=r::GlobalOnsetMethod::BeatEmphasis;
    r::PreparedGlobalOnsetRecipe operation;native_export::require(operation.prepare(c.input.size(),config,native_export::source_fourier_backend()),"global diagnostic prepare");
    native_export::require(operation.process(c.input,{&out,write_global}),"global diagnostic process");out.row<float>("curve.f32",operation.curve());weights(operation.band_weights(),out);
}
}
void diagnostics(const Case& c,const std::string& method) {
    native_export::Output out(c.prefix);
    if(method=="essentia_polar")polar(c,out);else if(method=="pulse_train")pulse(c,out);else if(method=="band_emphasis")emphasis(c,out);else global(c,out);
}
}
