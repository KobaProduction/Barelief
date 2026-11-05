#include "python_export.h"
#include <dsp_kernels/spectral_frame_recipe.h>
#include <algorithm>
#include <utility>

namespace originals {
namespace {
dsp_kernels::SpectralFrameRecipeConfig config(const Request& r) {
    dsp_kernels::SpectralFrameRecipeConfig c;
    c.framing.length=r.size(0);c.framing.hop_samples=r.number(1);c.fft_size=r.size(2);c.window_samples=r.size(3);
    c.periodic=r.size(4)!=0;c.framing.center=r.size(5)!=0;c.framing.tail=dsp_kernels::FrameSamplingTail(r.size(6));
    c.circular_shift=r.size(7)!=0;c.include_nyquist=r.size(8)!=0;c.window=dsp_kernels::WindowFunction(r.size(9));
    c.power=float(r.number(10));c.sample_rate=float(r.number(11));c.precision=dsp_kernels::NumericalPrecision(r.size(12));return c;
}
struct Rows {
    Matrix complex,magnitude,phase,powered,linear,features;
    Rows(std::size_t frames,std::size_t bins,std::size_t bands):complex(frames,bins*2),magnitude(frames,bins),phase(frames,bins),powered(frames,bins),linear(frames,bands),features(frames,bands){}
    static std::span<float> row(Matrix& matrix,std::size_t index) {return std::span(matrix.values).subspan(index*matrix.columns,matrix.columns);}
    bool frame(dsp_kernels::PreparedSpectralFrameRecipe& recipe,std::span<const float> input,std::size_t index) {
        if(!recipe.process_frame(input,index,{row(features,index),row(phase,index),row(magnitude,index),row(powered,index),row(linear,index)}))return false;
        const auto values=recipe.values().complex;std::copy(values.begin(),values.end(),row(complex,index).begin());return true;
    }
    void emit(Result& result,std::size_t frames) const {
        result.floats("complex",complex.values,{frames,magnitude.columns,2});
        result.floats("magnitude",magnitude.values,{frames,magnitude.columns});result.floats("phase",phase.values,{frames,phase.columns});
        result.floats("powered",powered.values,{frames,powered.columns});result.floats("linear",linear.values,{frames,linear.columns});
        result.floats("features",features.values,{frames,features.columns});
    }
};
}
Matrix spectral_stages(std::span<const float> input,const dsp_kernels::SpectralFrameRecipeConfig& config,Result& result) {
    dsp_kernels::PreparedSpectralFrameRecipe recipe;require(recipe.prepare(input.size(),config),"prepare spectral recipe");
    const auto workspace=recipe.workspace_bytes();result.integers("spectral_workspace_bytes",std::span(&workspace,1));
    const auto count=recipe.frames(input.size());Rows rows(count,recipe.phase_bins(),recipe.features());
    for(std::size_t index=0;index<count;++index)require(rows.frame(recipe,input,index),"spectral frame");
    rows.emit(result,count);result.floats("window",recipe.window_coefficients(),{recipe.window_coefficients().size()});
    result.floats("frequencies",recipe.frequencies_hz(),{recipe.phase_bins()});
    result.floats("source_filter_frequencies",recipe.source_filter_frequencies_hz(),{recipe.phase_bins()});return std::move(rows.features);
}
bool spectrum(const Request& r,const Matrix& input,Result& result) {
    (void)spectral_stages(input.values,config(r),result);return true;
}
}
