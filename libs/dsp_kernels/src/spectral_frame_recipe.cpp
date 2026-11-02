// SpectralOnsetProcessor frontend and audio/STFT/spectrogram semantics from
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/spectral_frame_recipe.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
namespace dsp_kernels {
namespace {
constexpr std::uint8_t complex_stage=1,magnitude_stage=2,phase_stage=4,powered_stage=8,linear_stage=16,feature_stage=32;
bool disjoint_outputs(std::span<const float> left,std::span<const float> right) noexcept {
    if(left.empty() || right.empty())return true;
    const std::less<const float*> before;
    return !before(left.data(),right.data()+right.size()) || !before(right.data(),left.data()+left.size());
}
bool valid_planes(std::span<const std::span<float>> planes) noexcept {
    bool any=false;
    for(std::size_t i=0;i<planes.size();++i){
        any=any || !planes[i].empty();
        for(std::size_t j=0;j<i;++j)if(!disjoint_outputs(planes[i],planes[j]))return false;
    }
    return any;
}
void copy_output(std::span<const float> values,std::span<float> output) noexcept {
    if(!output.empty())std::copy(values.begin(),values.end(),output.begin());
}
}
bool PreparedSpectralFrameRecipe::prepare(std::size_t maximum,SpectralFrameRecipeConfig config,
                                          std::span<const float> window,ExplicitSpectralFilters filters) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,window,filters,bound))return false;
    PreparedSpectralFrameRecipe next;
    if(!next.configure(maximum,config,window,filters) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedSpectralFrameRecipe::configure(std::size_t maximum,SpectralFrameRecipeConfig c,
                                            std::span<const float> window,ExplicitSpectralFilters filters) {
    if(!resolve_config(c) || unsigned(c.filtering)>unsigned(SpectralFilterMode::Mel) || !std::isfinite(c.power) ||
        unsigned(c.padding)>unsigned(TemporalPadding::Symmetric) ||
        unsigned(c.magnitude_norm)>unsigned(ComplexMagnitudeNorm::ScaledRatioFma) || unsigned(c.zero_phase)>unsigned(ZeroMagnitudePhase::Atan2))return false;
    if(!sampling_.prepare(maximum,c.framing))return false;
    config_=c;maximum_samples_=maximum;
    if(!prepare_spectrum(window) || !prepare_filter(filters))return false;
    magnitude_.resize(phase_bins());phase_.resize(phase_bins());powered_.resize(phase_bins());
    linear_.resize(features_);filtered_.resize(features_);return true;
}
bool PreparedSpectralFrameRecipe::prepare_window_coefficients(std::span<const float> supplied,std::vector<float>& output) const {
    const auto count=config_.window_samples?config_.window_samples:config_.framing.length;
    if(!count || count>config_.framing.length)return false;
    TemporalWindowConfig c;c.length=count;c.function=config_.window;c.periodic=config_.periodic;
    PreparedTemporalWindow window;if(!window.prepare(c,supplied))return false;
    output.assign(config_.framing.length,0.f);
    std::copy(window.coefficients().begin(),window.coefficients().end(),output.begin()+std::ptrdiff_t((output.size()-count)/2));
    return true;
}
TemporalWindowConfig PreparedSpectralFrameRecipe::spectrum_window() const noexcept {
    TemporalWindowConfig window;window.length=config_.framing.length;window.hop=1;
    window.precision=config_.precision;
    window.function=config_.window;window.periodic=config_.periodic;
    window.tail=TemporalTail::OriginInside;window.center=config_.framing.center;
    return window;
}
bool PreparedSpectralFrameRecipe::prepare_spectrum(std::span<const float> supplied) {
    const auto window=spectrum_window();
    const auto size=config_.fft_size?config_.fft_size:window.length;
    if(config_.circular_shift && (window.length%2 || size<window.length/2))return false;
    std::vector<float> coefficients;
    std::vector<double> reference;
    if(!prepare_window_coefficients(supplied,coefficients) || !prepare_reference_window_coefficients(supplied,reference) ||
       !spectrum_.prepare_size(window,coefficients,size,config_.circular_shift,reference))return false;
    return prepare_spectrum_axes(size);
}
bool PreparedSpectralFrameRecipe::prepare_spectrum_axes(std::size_t size) {
    const auto bins=size/2+std::size_t(config_.include_nyquist);
    if(!bins)return false;
    frequencies_.resize(bins);source_filter_frequencies_.resize(bins);
    for(std::size_t i=0;i<bins;++i){
        frequencies_[i]=float(i)*config_.sample_rate/float(size);
        source_filter_frequencies_[i]=float(i)*config_.sample_rate/(2.f*float(bins));
    }
    complex_.resize(2*spectrum_.bins());return true;
}
bool PreparedSpectralFrameRecipe::prepare_reference_window_coefficients(std::span<const float> supplied,std::vector<double>& output) const {
    if(config_.precision!=NumericalPrecision::DesktopReference)return true;
    const auto count=config_.window_samples?config_.window_samples:config_.framing.length;
    if(!count || count>config_.framing.length || (!supplied.empty() && supplied.size()!=count))return false;
    output.assign(config_.framing.length,0.);const auto offset=(output.size()-count)/2;
    for(std::size_t i=0;i<count;++i)
        output[offset+i]=supplied.empty()?window_coefficient_value<double>(i,count,config_.window,config_.periodic):double(supplied[i]);
    return true;
}
bool PreparedSpectralFrameRecipe::explicit_filter(std::size_t bands,std::span<const float> weights,std::span<const double> centers) {
    if(!bands || bands>SIZE_MAX/phase_bins() || weights.size()!=bands*phase_bins())return false;
    supports_.resize(bands);
    for(std::size_t band=0;band<bands;++band){
        const auto row=weights.subspan(band*phase_bins(),phase_bins());std::size_t first=0,last=row.size();
        while(first<last && row[first]==0)++first;
        while(last>first && row[last-1]==0)--last;
        supports_[band]={first,last-first};
    }
    // Full projection retains source matrix semantics, including all-zero bands.
    if(!filtering_.prepare_matrix(phase_bins(),bands,weights,centers,{false,false}))return false;
    features_=bands;return true;
}
bool PreparedSpectralFrameRecipe::prepare_mel_filter() {
    auto config=config_.mel_filters;config.sample_rate=config_.sample_rate;config.first_bin=0;
    const auto fft_size=config_.fft_size?config_.fft_size:config_.framing.length;
    config.bin_hz=double(config_.sample_rate)/double(config_.physical_filter_axis?fft_size:2*phase_bins());
    std::vector<double> grid(source_filter_frequencies_.begin(),source_filter_frequencies_.end());
    if(config_.physical_filter_axis)for(std::size_t i=0;i<grid.size();++i)grid[i]=double(i)*config.bin_hz;
    MelFilterBank bank;if(!bank.prepare(phase_bins(),config,grid))return false;
    return explicit_filter(bank.centers_hz().size(),bank.weights(),bank.centers_hz());
}
bool PreparedSpectralFrameRecipe::prepare_filter(ExplicitSpectralFilters supplied) {
    features_=phase_bins();
    if(config_.filtering==SpectralFilterMode::None)return true;
    if(config_.filtering==SpectralFilterMode::Explicit)return explicit_filter(supplied.bands,supplied.weights,supplied.centers_hz);
    if(config_.filtering==SpectralFilterMode::Mel)return prepare_mel_filter();
    LogFrequencyFilterBank bank;
    if(!bank.prepare(config_.physical_filter_axis?std::span<const float>(frequencies_):std::span<const float>(source_filter_frequencies_),config_.logarithmic_filters))return false;
    const std::vector<double> centers(bank.centers_hz().begin(),bank.centers_hz().end());
    return explicit_filter(bank.bands(),bank.weights(),centers);
}
bool PreparedSpectralFrameRecipe::filter_values() noexcept {
    if(config_.filtering==SpectralFilterMode::None)std::copy(powered_.begin(),powered_.end(),linear_.begin());
    else if(!filtering_.process(powered_,linear_))return false;
    return std::all_of(linear_.begin(),linear_.end(),[](float x){return std::isfinite(x);});
}
bool PreparedSpectralFrameRecipe::feature_values() noexcept {
    std::copy(linear_.begin(),linear_.end(),filtered_.begin());
    if(config_.logarithm && !logarithmic_compression(filtered_,filtered_,config_.logarithm_multiply,
        config_.logarithm_add,config_.logarithm_base,true))return false;
    if(!std::all_of(filtered_.begin(),filtered_.end(),[](float x){return std::isfinite(x);}))return false;
    return true;
}
bool PreparedSpectralFrameRecipe::process_frame(std::span<const float> pcm,std::size_t frame,
                                                std::span<float> output,std::span<float> phase) noexcept {
    return process_frame(pcm,frame,SpectralFrameOutputs{output,phase});
}
bool PreparedSpectralFrameRecipe::process_frame(std::span<const float> pcm,std::size_t frame,SpectralFrameOutputs outputs) noexcept {
    return transform_frame(pcm,frame) && project_frame(std::span<const float>(complex_).first(2*phase_bins()),outputs);
}
bool PreparedSpectralFrameRecipe::transform_frame(std::span<const float> pcm,std::size_t frame) noexcept {
    stages_=0;
    if(!maximum_samples_ || pcm.size()>maximum_samples_)return false;
    std::ptrdiff_t first=0;
    if(!sampling_.first_sample(pcm.size(),frame,first) || !spectrum_.process_at(pcm,first,complex_,0,config_.padding))return false;
    stages_=complex_stage;return true;
}
bool PreparedSpectralFrameRecipe::valid_outputs(SpectralFrameOutputs output) const noexcept {
    const std::array planes{output.features,output.phase,output.magnitude,output.powered,output.linear};
    if(!valid_planes(planes))return false;
    return (output.features.empty() || output.features.size()==features_) &&
        (output.linear.empty() || output.linear.size()==features_) &&
        (output.phase.empty() || output.phase.size()==phase_bins()) &&
        (output.magnitude.empty() || output.magnitude.size()==phase_bins()) &&
        (output.powered.empty() || output.powered.size()==phase_bins());
}
bool PreparedSpectralFrameRecipe::calculate_values(SpectralFrameOutputs output,std::uint8_t& stages) noexcept {
    const bool power=!output.powered.empty() || !output.linear.empty() || !output.features.empty();
    const bool magnitude=power || !output.magnitude.empty(),phase=!output.phase.empty();
    const auto norm=config_.precision==NumericalPrecision::DesktopReference && config_.magnitude_norm==ComplexMagnitudeNorm::Hypot?
        ComplexMagnitudeNorm::ScaledRatioFma:config_.magnitude_norm;
    if(!cartesian_to_polar(std::span<const float>(complex_).first(2*phase_bins()),magnitude?std::span(magnitude_):std::span<float>{},
        phase?std::span(phase_):std::span<float>{},config_.zero_phase,norm))return false;
    if(magnitude)stages|=magnitude_stage;
    if(phase)stages|=phase_stage;
    return !power || calculate_derived(output,stages);
}
bool PreparedSpectralFrameRecipe::calculate_derived(SpectralFrameOutputs output,std::uint8_t& stages) noexcept {
    if(!magnitude_power(magnitude_,config_.power,powered_))return false;
    stages|=powered_stage;
    if(!output.linear.empty() || !output.features.empty()){
        if(!filter_values())return false;
        stages|=linear_stage;
    }
    if(!output.features.empty()){if(!feature_values())return false;stages|=feature_stage;}
    return true;
}
void PreparedSpectralFrameRecipe::publish(SpectralFrameOutputs output) const noexcept {
    copy_output(filtered_,output.features);copy_output(phase_,output.phase);copy_output(magnitude_,output.magnitude);
    copy_output(powered_,output.powered);copy_output(linear_,output.linear);
}
bool PreparedSpectralFrameRecipe::project_frame(std::span<const float> raw,SpectralFrameOutputs output) noexcept {
    stages_=0;
    if(!maximum_samples_ || raw.size()!=2*phase_bins() || !valid_outputs(output))return false;
    if(raw.data()!=complex_.data()){
        if(!disjoint_outputs(raw,complex_))return false;
        std::copy(raw.begin(),raw.end(),complex_.begin());
    }
    std::uint8_t stages=complex_stage;
    if(!calculate_values(output,stages))return false;
    publish(output);stages_=stages;return true;
}
SpectralFrameValues PreparedSpectralFrameRecipe::values() const noexcept {
    const auto selected=[this](std::uint8_t stage,const std::vector<float>& values){
        return stages_&stage?std::span<const float>(values):std::span<const float>{};
    };
    return {stages_&complex_stage?std::span<const float>(complex_).first(2*phase_bins()):std::span<const float>{},
        selected(magnitude_stage,magnitude_),selected(phase_stage,phase_),selected(powered_stage,powered_),
        selected(linear_stage,linear_),selected(feature_stage,filtered_)};
}
std::size_t PreparedSpectralFrameRecipe::workspace_bytes() const noexcept {
    return (complex_.capacity()+magnitude_.capacity()+phase_.capacity()+powered_.capacity()+linear_.capacity()+
        filtered_.capacity()+frequencies_.capacity()+source_filter_frequencies_.capacity())*sizeof(float)+
        supports_.capacity()*sizeof(FilterBandSupport)+spectrum_.workspace_bytes()+filtering_.workspace_bytes();
}
}
