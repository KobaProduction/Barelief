#include <dsp_kernels/spectral_frame_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
namespace {
struct Grid {float rate;std::size_t fft,bins;bool physical,mel;};
double frequency(const void* context,std::size_t bin) noexcept {
    const auto& g=*static_cast<const Grid*>(context);
    if(g.physical && g.mel)return double(bin)*(double(g.rate)/double(g.fft));
    return float(bin)*g.rate/float(g.physical?g.fft:2*g.bins);
}
bool shape(const SpectralFrameRecipeConfig& c,std::span<const float> window,std::size_t& fft,std::size_t& bins) noexcept {
    fft=c.fft_size?c.fft_size:c.framing.length;
    const auto length=c.window_samples?c.window_samples:c.framing.length;
    if(!fft || !length || length>c.framing.length || (!window.empty() && window.size()!=length) ||
       !std::isfinite(c.power) ||
       unsigned(c.filtering)>unsigned(SpectralFilterMode::Mel) || unsigned(c.padding)>unsigned(TemporalPadding::Symmetric))return false;
    bins=fft/2+std::size_t(c.include_nyquist);return bins!=0;
}
bool filter_capacity(const SpectralFrameRecipeConfig& c,std::size_t bins,ExplicitSpectralFilters filters,std::size_t& features) noexcept {
    if(c.filtering==SpectralFilterMode::Explicit){
        features=filters.bands;
        if(!features || features>SIZE_MAX/bins || filters.weights.size()!=features*bins ||
           (!filters.centers_hz.empty() && filters.centers_hz.size()!=features))return false;
    }else if(c.filtering==SpectralFilterMode::Mel)features=c.mel_filters.bands;
    else {
        const auto& f=c.logarithmic_filters;
        if(!std::isfinite(f.fmin) || !std::isfinite(f.fmax) || !std::isfinite(f.reference) || !std::isfinite(f.bands_per_octave) ||
           f.fmin<=0 || f.fmax<=f.fmin || f.reference<=0 || f.bands_per_octave<=0)return false;
        const double count=std::ceil(std::log2(double(f.fmax)/f.reference)*f.bands_per_octave)-std::floor(std::log2(double(f.fmin)/f.reference)*f.bands_per_octave);
        if(!std::isfinite(count) || count<3 || count>=double(SIZE_MAX/sizeof(std::size_t)))return false;
        features=std::size_t(count);
    }
    return true;
}
bool filter_bound(const SpectralFrameRecipeConfig& c,std::size_t bins,ExplicitSpectralFilters filters,
                  std::size_t& features,std::size_t& bytes) noexcept {
    features=bins;bytes=0;
    if(c.filtering==SpectralFilterMode::None)return true;
    std::size_t bank=0;
    if(!filter_capacity(c,bins,filters,features) || !MelFilterBank::matrix_storage_bytes(bins,features,bank))return false;
    // Cold filter construction and the final explicit matrix coexist. Log
    // bank push_back vectors are bounded by twice the requested corner count.
    bytes=bank;
    return prepared_storage::add_bytes(bytes,bank) && prepared_storage::add_array(bytes,features,4*(sizeof(std::size_t)+sizeof(FilterBandSupport)+sizeof(double))) &&
        prepared_storage::add_array(bytes,bins,sizeof(double));
}
}
bool PreparedSpectralFrameRecipe::resolve_config(SpectralFrameRecipeConfig& c) noexcept {
    if(unsigned(c.precision)>unsigned(NumericalPrecision::DesktopReference))return false;
    if(!std::isfinite(c.sample_rate) || c.sample_rate<=0 || !std::isfinite(c.frame_rate) || c.frame_rate<0)return false;
    if(c.frame_rate>0)c.framing.hop_samples=double(c.sample_rate)/c.frame_rate;
    return std::isfinite(c.framing.hop_samples) && c.framing.hop_samples>0;
}
bool PreparedSpectralFrameRecipe::geometry(std::size_t maximum,SpectralFrameRecipeConfig c,
    ExplicitSpectralFilters filters,std::size_t& frames,std::size_t& features) noexcept {
    std::size_t fft=0,bins=0;
    if(!resolve_config(c) || !shape(c,{},fft,bins))return false;
    if(!PreparedFrameSampling::frame_count(maximum,c.framing,frames))return false;
    if(c.filtering==SpectralFilterMode::None){features=bins;return true;}
    if(c.filtering==SpectralFilterMode::Explicit){features=filters.bands;return features && features<=SIZE_MAX/bins && filters.weights.size()==features*bins;}
    Grid grid{c.sample_rate,fft,bins,c.physical_filter_axis,c.filtering==SpectralFilterMode::Mel};
    if(c.filtering==SpectralFilterMode::Logarithmic)return LogFrequencyFilterBank::band_count({&grid,bins,frequency},c.logarithmic_filters,features);
    auto mel=c.mel_filters;mel.sample_rate=c.sample_rate;mel.first_bin=0;
    mel.bin_hz=double(c.sample_rate)/double(c.physical_filter_axis?fft:2*bins);
    return MelFilterBank::band_count({&grid,bins,frequency},mel,features);
}
bool PreparedSpectralFrameRecipe::storage_bytes(std::size_t maximum,SpectralFrameRecipeConfig c,
    std::span<const float> window,ExplicitSpectralFilters filters,std::size_t& output) noexcept {
    std::size_t fft=0,bins=0,features=0,bytes=0,nested=0,frames=0,resolved=0;
    if(!resolve_config(c) || !shape(c,window,fft,bins) || !geometry(maximum,c,filters,frames,resolved) || !filter_bound(c,bins,filters,features,bytes))return false;
    TemporalWindowConfig framing;framing.length=c.framing.length;framing.center=c.framing.center;
    framing.precision=c.precision;
    framing.function=c.window;framing.periodic=c.periodic;framing.tail=TemporalTail::OriginInside;
    if(!PreparedFourierTempogram::storage_bytes(framing,{},fft,c.circular_shift,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !prepared_storage::add_array(bytes,fft/2+1,2*sizeof(float)) || !prepared_storage::add_array(bytes,bins,5*sizeof(float)) ||
       !prepared_storage::add_array(bytes,features,2*sizeof(float)+sizeof(FilterBandSupport)) ||
       !prepared_storage::add_array(bytes,c.framing.length,2*sizeof(float)))return false;
    if(c.precision==NumericalPrecision::DesktopReference && !prepared_storage::add_array(bytes,c.framing.length,2*sizeof(double)))return false;
    output=bytes;return true;
}
}
