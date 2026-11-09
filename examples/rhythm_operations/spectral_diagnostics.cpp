// Bounded lifecycle regressions for the optional composition consumer.
// These check validity of current-call diagnostics, not numerical parity.
#include <dsp_kernels/spectral_novelty.h>
#include <dsp_kernels/spectral_frame_recipe.h>
#include <algorithm>
#include <array>
#include <limits>

namespace {
using namespace dsp_kernels;
bool source_row(const void*,std::size_t frame,std::span<float> values) noexcept {
    for(std::size_t bin=0;bin<values.size();++bin)values[bin]=float((frame+1)*(bin+1));
    return true;
}
bool accept_row(void*,std::size_t,std::span<const float>) noexcept {return true;}
bool empty_diagnostics(const PreparedSpectralNovelty& kernel) noexcept {
    return kernel.difference_values().empty() && kernel.phase_mask_values().empty() &&
        kernel.alignment_correlations().empty() && kernel.alignment_offset()==0;
}
bool novelty_diagnostics(SpectralNoveltyMethod method) {
    PreparedSpectralNovelty kernel;SpectralNoveltyConfig config;config.method=method;config.lag=2;
    if(!kernel.prepare(8,4,4,config))return false;
    const FloatMatrixSource source{nullptr,source_row};const FloatMatrixDestination target{nullptr,accept_row};
    if(!kernel.process(source,source,8,target) || empty_diagnostics(kernel))return false;
    if(!kernel.process(source,source,0,target) || !empty_diagnostics(kernel))return false;
    return kernel.process(source,source,config.lag,target) && empty_diagnostics(kernel);
}
bool empty_spectrum(const PreparedSpectralFrameRecipe& kernel) noexcept {
    const auto value=kernel.values();
    return value.complex.empty() && value.magnitude.empty() && value.phase.empty() &&
        value.powered.empty() && value.linear.empty() && value.features.empty();
}
SpectralFrameRecipeConfig diagnostic_recipe() {
    SpectralFrameRecipeConfig config;
    config.framing.length=8;
    config.framing.hop_samples=4;
    config.framing.center=false;
    config.framing.tail=FrameSamplingTail::Complete;
    return config;
}
bool nonfinite_pcm_diagnostics(PreparedSpectralFrameRecipe& kernel,std::span<float> features) {
    std::array<float,16> pcm;
    pcm.fill(.25f);
    if(!kernel.process_frame(pcm,0,features))return false;
    pcm[0]=std::numeric_limits<float>::quiet_NaN();
    return !kernel.process_frame(pcm,0,features) && empty_spectrum(kernel);
}

bool pcm_diagnostics() {
    PreparedSpectralFrameRecipe kernel;
    if(!kernel.prepare(16,diagnostic_recipe()))return false;
    std::array<float,16> pcm;
    pcm.fill(.25f);
    std::vector<float> features(kernel.features());
    if(!kernel.process_frame(pcm,0,features) || empty_spectrum(kernel))return false;
    if(kernel.process_frame({},0,features) || !empty_spectrum(kernel))return false;
    return nonfinite_pcm_diagnostics(kernel,features);
}
}
bool spectral_diagnostic_regressions() {
    return novelty_diagnostics(dsp_kernels::SpectralNoveltyMethod::PositiveDifference) &&
        novelty_diagnostics(dsp_kernels::SpectralNoveltyMethod::PhaseMaskedDifference) &&
        novelty_diagnostics(dsp_kernels::SpectralNoveltyMethod::AlignedDifference) && pcm_diagnostics();
}
