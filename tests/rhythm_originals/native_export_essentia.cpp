#include "source_fourier_profile.h"
#include "native_export_io.h"
#include <rhythm_reference_ports/spectral_window.h>
#include <rhythm_reference_ports/spectral_onset.h>
#include <rhythm_reference_ports/global_onset_recipe.h>
#include <dsp_kernels/complex_values.h>
#include <dsp_kernels/tempogram.h>
#include <array>
#include <limits>
namespace native_export {
namespace {
namespace r=rhythm_reference_ports;
struct Analysis {
    dsp_kernels::PreparedFourierTempogram spectrum;
    std::array<r::PreparedSpectralOnset,6> onset;
    std::array<bool,6> available{};
    std::vector<float> coefficients,complex,magnitude,phase;
};
void prepare(Analysis& a,const Case& c,Output& out) {
    a.coefficients.resize(c.frame);a.complex.resize((c.frame/2+1)*2);
    a.magnitude.resize(c.frame/2+1);a.phase.resize(c.frame/2+1);
    r::SpectralWindowConfig window;window.precision=dsp_kernels::NumericalPrecision::DesktopReference;window.shape=r::SpectralWindowShape(c.window);
    require(c.window<9 && r::spectral_window(a.coefficients,window),"Essentia window prepare");
    dsp_kernels::TemporalWindowConfig framing;framing.length=c.frame;framing.hop=c.hop;framing.center=false;
    require(a.spectrum.prepare(framing,a.coefficients,0,true,native_export::source_fourier_backend()),"Essentia FFT prepare");
    for(std::size_t i=0;i<a.onset.size();++i){
        a.available[i]=a.onset[i].prepare(a.magnitude.size(),{r::SpectralOnsetMethod(i),float(c.rate)});
        require(a.available[i] || i==4,"Essentia ODF prepare");
    }
    out.row<float>("window.f32",a.coefficients);
}
void frame(Analysis& a,const Case& c,std::size_t index,Output& out) {
    require(a.spectrum.process_frame(c.pcm,index,a.complex),"Essentia FFT process");
    require(dsp_kernels::cartesian_to_polar(a.complex,a.magnitude,a.phase,dsp_kernels::ZeroMagnitudePhase::Atan2,
        dsp_kernels::ComplexMagnitudeNorm::SquaredNorm),"Essentia polar process");
    out.row<float>("norm.f32",a.magnitude);out.row<float>("phase.f32",a.phase);
    out.row<float>("complex.f32",a.complex);out.row<float>("transform.f32",a.spectrum.view().transform_input);
    for(std::size_t i=0;i<a.onset.size();++i){
        float value=std::numeric_limits<float>::quiet_NaN();
        if(a.available[i])require(a.onset[i].process(a.magnitude,a.phase,value),"Essentia ODF process");
        out.values<float>("odf.f32",{value});
        out.values<std::uint64_t>("odf_status.u64",{a.available[i]?0u:1u});
    }
}
void globals(const Case& c,Output& out) {
    for(unsigned i=0;i<2;++i){
        r::PreparedGlobalOnsetRecipe recipe;
        r::GlobalOnsetRecipeConfig config{float(c.rate),c.frame,c.hop,r::GlobalOnsetMethod(i)};config.window_precision=dsp_kernels::NumericalPrecision::DesktopReference;
        require(recipe.prepare(c.pcm.size(),config,native_export::source_fourier_backend()),"Essentia global prepare");
        require(recipe.process(c.pcm),"Essentia global process");
        out.row<float>(i?"global_beat_emphasis.f32":"global_infogain.f32",recipe.curve());
    }
}
}
void essentia(const Case& c) {
    Output output(c.prefix);Analysis analysis;prepare(analysis,c,output);
    for(std::size_t i=0;i<analysis.spectrum.frames(c.pcm.size());++i)frame(analysis,c,i,output);
    // Globals have their own fixed normalized Hann recipe, independent of the
    // per-frame ODF window choice above.
    globals(c,output);
}
}
