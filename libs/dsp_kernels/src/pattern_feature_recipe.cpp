// Adapted from madmom bin/GMMPatternTracker and audio/spectrogram.py at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/pattern_feature_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
SpectralFrameRecipeConfig PreparedPatternFeatureRecipe::parameters() noexcept {
    SpectralFrameRecipeConfig config;config.framing.length=2048;config.framing.hop_samples=882;
    config.filtering=SpectralFilterMode::Logarithmic;config.logarithmic_filters.bands_per_octave=12;
    config.logarithmic_filters.normalize=false;config.logarithm=true;return config;
}
bool PreparedPatternFeatureRecipe::bands() {
    const auto source_centers=spectrum_.source_filter_centers_hz();
    const std::vector<float> centers(source_centers.begin(),source_centers.end());
    constexpr std::array<float,3> borders{30,270,17000};
    if(!grouping_.prepare(centers,borders,false,true))return false;
    filtered_.resize(spectrum_.features());previous_.resize(spectrum_.features());difference_.resize(spectrum_.features());
    grouped_.resize(grouping_.bands());return true;
}
bool PreparedPatternFeatureRecipe::configure(std::size_t maximum) {
    if(!maximum || maximum>std::size_t(std::numeric_limits<int>::max()))return false;
    const auto config=parameters();
    std::vector<float> coefficients(2048);constexpr std::array<float,2> weights{.5f,-.5f};
    if(!cosine_sum_window(coefficients,weights,2047) || !spectrum_.prepare(maximum,config,coefficients) || !bands())return false;
    std::size_t lag=0;
    if(!spectral_difference_lag(coefficients,882,.5f,lag) || lag!=1)return false;
    OnsetStrengthConfig difference;difference.center=false;difference.n_fft=2048;difference.hop_length=882;
    if(!differences_.prepare(spectrum_.features(),spectrum_.frames(maximum),difference))return false;
    maximum_pcm_=maximum;return true;
}
bool PreparedPatternFeatureRecipe::prepare(std::size_t maximum) {
    std::size_t bound=0;if(!storage_bytes(maximum,bound))return false;
    PreparedPatternFeatureRecipe next;
    if(!next.configure(maximum))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedPatternFeatureRecipe::reset() noexcept {
    next_frame_=0;std::fill(previous_.begin(),previous_.end(),0.f);
}
bool PreparedPatternFeatureRecipe::difference() noexcept {
    if(!next_frame_)std::fill(difference_.begin(),difference_.end(),0.f);
    else if(!differences_.positive_differences(filtered_,previous_,difference_,false))return false;
    std::copy(filtered_.begin(),filtered_.end(),previous_.begin());return true;
}
bool PreparedPatternFeatureRecipe::read_frame(std::span<const float> pcm,std::size_t frame,std::span<float> output) noexcept {
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_ || frame!=next_frame_ || output.size()!=features())return false;
    if(!spectrum_.process_frame(pcm,frame,filtered_) || !difference() || !grouping_.process(difference_,grouped_))return false;
    if(output.data()!=grouped_.data())std::copy(grouped_.begin(),grouped_.end(),output.begin());
    ++next_frame_;return true;
}
bool PreparedPatternFeatureRecipe::process(std::span<const float> pcm,FloatMatrixDestination output) noexcept {
    reset();
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_ || !output.write ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t frame=0;frame<frames(pcm.size());++frame)
        if(!read_frame(pcm,frame,grouped_) || !output.write(output.context,frame,grouped_))return false;
    return true;
}
}

namespace dsp_kernels {
bool PreparedPatternFeatureRecipe::geometry(std::size_t maximum,std::size_t& frames,std::size_t& features) noexcept {
    std::size_t bins=0;
    if(!maximum || maximum>INT32_MAX || !PreparedSpectralFrameRecipe::geometry(maximum,parameters(),{},frames,bins) || bins<2)return false;
    features=2;return true;
}
bool PreparedPatternFeatureRecipe::storage_bytes(std::size_t maximum,std::size_t& out) noexcept {
    std::size_t frames=0,features=0,bins=0,bytes=0,nested=0;
    if(!geometry(maximum,frames,features) || !PreparedSpectralFrameRecipe::geometry(maximum,parameters(),{},frames,bins) ||
       !PreparedSpectralFrameRecipe::storage_bytes(maximum,parameters(),{},{},bytes) ||
       !PreparedRectangularFilterBank::storage_bytes(bins,3,nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    OnsetStrengthConfig difference;difference.center=false;difference.n_fft=2048;difference.hop_length=882;
    if(!PreparedOnsetStrength::storage_bytes(bins,frames,difference,nested))return false;
    nested-=sizeof(PreparedOnsetStrength);
    if(!prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,bins,4*sizeof(float)) ||
       !prepared_storage::add_array(bytes,features,sizeof(float)) || !prepared_storage::add_array(bytes,2048,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedPatternFeatureRecipe::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+grouping_.workspace_bytes()+differences_.workspace_bytes()+
        (filtered_.capacity()+previous_.capacity()+difference_.capacity()+grouped_.capacity())*sizeof(float);
}
}
