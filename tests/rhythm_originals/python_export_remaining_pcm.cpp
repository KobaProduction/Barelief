#include "python_export_remaining.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/pcm_spectral_novelty.h>

namespace originals {
dsp_kernels::PcmSpectralNoveltyConfig recipe_pcm_novelty(const Request& r) {
    dsp_kernels::PcmSpectralNoveltyConfig c;
    c.spectrum.sample_rate=8000;c.spectrum.framing.length=64;c.spectrum.framing.hop_samples=17;c.spectrum.fft_size=64;
    c.novelty.method=dsp_kernels::SpectralNoveltyMethod(r.size(0));c.spectrum.precision=dsp_kernels::NumericalPrecision(r.size(1));
    return c;
}
bool remaining_pcm(const Request& r,const Matrix& input,Result& result) {
    require(r.method=="pcm_novelty","unknown remaining method");const auto c=recipe_pcm_novelty(r);
    dsp_kernels::PreparedPcmSpectralNovelty kernel;std::size_t bound=0;
    require(decltype(kernel)::storage_bytes(input.values.size(),c,{},{},bound),"query full PCM novelty cold bound");
    require(benchmark::measured_prepare(bound,[&]{return kernel.prepare(input.values.size(),c);}),"prepare full PCM spectral novelty");
    Matrix output(kernel.frames(input.values.size()),kernel.output_features());
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output.destination());});
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});
    auto trace=c.spectrum;if(dsp_kernels::spectral_novelty_uses_phase(c.novelty.method))trace.circular_shift=true;
    (void)spectral_stages(input.values,trace,result);
    report_workspace(result,kernel.workspace_bytes());return ok;
}
}
