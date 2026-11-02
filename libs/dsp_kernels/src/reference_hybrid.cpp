#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/complex_values.h>
#include <dsp_kernels/detail/source_spectral_math.h>
#include <dsp_kernels/detail/float_publication.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
bool PreparedTempogramSequence::prepare_reference_hybrid() {
    const auto length=config_.autocorrelation.window.length;
    if(length<3 || config_.interpolation.kind!=CurveInterpolation::Linear)return false;
    reference_coordinates_.resize(length-1);
    for(std::size_t i=0;i<reference_coordinates_.size();++i)
        reference_coordinates_[i]=60.*double(config_.frame_rate)/double(length-1-i);
    return true;
}
bool PreparedTempogramSequence::interpolate_reference(double query,double& value) const noexcept {
    const auto& grid=reference_coordinates_;const auto& c=config_.interpolation;
    const bool below=query<grid.front(),above=query>grid.back();
    if((below || above) && c.bounds_error)return false;
    if((below || above) && !c.extrapolate){value=below?double(c.fill_below):double(c.fill_above);return true;}
    const auto at=std::lower_bound(grid.begin(),grid.end(),query);
    const auto upper=std::clamp(std::size_t(at-grid.begin()),std::size_t{1},grid.size()-1);
    const auto acf=autocorrelation_.reference_values();
    if(acf.size()!=grid.size()+1)return false;
    value=detail::source_linear_segment(grid[upper-1],grid[upper],acf[acf.size()-upper],acf[acf.size()-1-upper],query);
    return std::isfinite(value);
}
bool PreparedTempogramSequence::combine_reference_hybrid() noexcept {
    if(reference_coordinates_.empty())return false;
    for(std::size_t bin=0;bin<row_.size();++bin){
        double sample=0;const double query=60.*double(config_.frame_rate)*double(bin)/double(config_.autocorrelation.window.length);
        if(!interpolate_reference(query,sample))return false;
        float magnitude=0;
        if(!complex_polar_value(fourier_row_[2*bin],fourier_row_[2*bin+1],&magnitude,nullptr,
            {ZeroMagnitudePhase::Zero,ComplexMagnitudeNorm::ScaledRatioFma,false}))return false;
        const double value=std::sqrt(std::max(0.,double(magnitude)*std::abs(sample)));
        if(!detail::finite_float_range(value))return false;
        row_[bin]=float(value);
    }
    return true;
}
}
