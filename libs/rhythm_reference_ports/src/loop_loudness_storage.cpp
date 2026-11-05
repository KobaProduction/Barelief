// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/loop_tempo_recipe.h>
#include <rhythm_reference_ports/beat_loudness_sequence.h>
#include <rhythm_reference_ports/beat_band_salience.h>
#include <rhythm_reference_ports/correlation_meter.h>
#include <rhythm_reference_ports/band_rhythm_transform.h>
#include <rhythm_reference_ports/activity_fluctuation_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
using namespace dsp_kernels::prepared_storage;
namespace {
bool band_storage(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept {
    if(!bins || bins>INT_MAX || !std::isfinite(rate) || rate<=0 || rate/2<=0 || borders.empty())return false;
    float previous=0;
    for(float value:borders){if(!std::isfinite(value) || value<previous)return false;previous=value;}
    const auto bands=borders.size()-1;std::size_t weights=0;
    bytes=0;if(!bands)return true;
    return MelFilterBank::matrix_storage_bytes(bins,bands,bytes) && mul_bytes(bins,bands,weights) &&
        add_array(bytes,weights,sizeof(float)) && add_array(bytes,bands,sizeof(double)+sizeof(FilterBandSupport));
}
TemporalWindowConfig beat_window(std::size_t size,std::size_t hop) noexcept {
    TemporalWindowConfig window;window.length=size;window.hop=hop;window.center=false;window.periodic=false;return window;
}
}
bool PreparedLoopLengthConfidence::storage_bytes(float rate,std::size_t& bytes) noexcept {
    bytes=0;return std::isfinite(rate) && rate>=1 && rate<=std::nextafter(float(INT_MAX),0.f);
}
bool PreparedLoopTempoRecipe::storage_bytes(std::size_t maximum,float threshold,std::size_t& bytes,dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    PulseTempoRecipeConfig config;config.window_precision=precision;
    return std::isfinite(threshold) && threshold>=0 && threshold<=1 && PreparedPulseTempoRecipe::storage_bytes(maximum,config,{},bytes,factory);
}
bool PreparedFrequencyBandEnergy::storage_bytes(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept {
    return bins>=2 && borders.size()>=2 && std::adjacent_find(borders.begin(),borders.end())==borders.end() && band_storage(bins,rate,borders,bytes);
}
bool PreparedSpectralBandRatios::storage_bytes(std::size_t bins,float rate,std::span<const float> borders,std::size_t& bytes) noexcept {
    std::size_t descriptor=0;
    return band_storage(bins,rate,borders,bytes) && PreparedSpectralDescriptor::storage_bytes(bins,descriptor) && add_bytes(bytes,descriptor) && add_array(bytes,borders.size()-1,sizeof(float));
}
bool PreparedBeatSegmentEnergy::storage_bytes(BeatSegmentEnergyConfig config,std::span<const float> borders,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(config.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    PreparedBeatSegmentEnergy shape;if(!shape.geometry(config))return false;
    const auto n=shape.energy_samples_,bins=n/2+1;std::size_t bank=0;
    return PreparedFourierTempogram::storage_bytes(beat_window(n,n),{},n,false,bytes,factory) &&
        PreparedSpectralBandRatios::storage_bytes(bins,shape.effective_rate_,borders,bank) && add_bytes(bytes,bank) &&
        add_array(bytes,n,2*sizeof(float)) && add_array(bytes,bins,3*sizeof(float)) && add_array(bytes,borders.size()-1,sizeof(float));
}
std::size_t PreparedBeatSegmentEnergy::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+bands_.workspace_bytes()+(ones_.capacity()+complex_.capacity()+magnitude_.capacity()+ratios_.capacity())*sizeof(float);
}
bool PreparedBeatLoudnessSequence::storage_bytes(std::size_t maximum,BeatLoudnessSequenceConfig config,std::span<const float> borders,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(config.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    std::size_t rows=0;BeatSegmentEnergyConfig beat;beat.sample_rate=config.sample_rate;beat.search_seconds=config.search_seconds;beat.energy_seconds=config.energy_seconds;beat.window_precision=config.window_precision;
    return maximum && PreparedBeatSegmentEnergy::storage_bytes(beat,borders,bytes,factory) && mul_bytes(maximum,borders.size()-1,rows) &&
        add_array(bytes,maximum,sizeof(Slice)+sizeof(float)+sizeof(std::size_t)) && add_array(bytes,rows,sizeof(float));
}
std::size_t PreparedBeatLoudnessSequence::workspace_bytes() const noexcept {
    return beat_.workspace_bytes()+slices_.capacity()*sizeof(Slice)+indices_.capacity()*sizeof(std::size_t)+(energies_.capacity()+ratios_.capacity())*sizeof(float);
}
bool PreparedBeatBandSalience::storage_bytes(std::size_t beats,std::size_t bands,std::size_t window,std::size_t& bytes) noexcept {
    std::size_t cells=0;bytes=0;
    return beats && bands && window && beats<=std::size_t(PTRDIFF_MAX)/2 && mul_bytes(beats,bands,cells) &&
        add_array(bytes,cells,sizeof(float)) && add_array(bytes,bands,sizeof(float)) && add_array(bytes,std::min(beats,window),sizeof(float)) && add_array(bytes,beats,2*sizeof(float));
}
std::size_t PreparedBeatBandSalience::workspace_bytes() const noexcept {
    return (values_.capacity()+row_.capacity()+scratch_.capacity()+tick_thresholds_.capacity()+band_thresholds_.capacity())*sizeof(float);
}
bool PreparedCorrelationMeter::storage_bytes(std::size_t beats,std::size_t bands,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    return beats>=3 && beats<=INT_MAX && bands && PreparedVariableAutocorrelation::storage_bytes(beats,CorrelationNormalization::Raw,bytes,factory) &&
        add_array(bytes,beats,4*sizeof(float)) && add_array(bytes,bands,sizeof(float)+sizeof(std::size_t));
}
std::size_t PreparedCorrelationMeter::workspace_bytes() const noexcept {
    return correlation_.workspace_bytes()+(row_.capacity()+acf_.capacity()+sum_.capacity()+histogram_.capacity()+band_maxima_.capacity())*sizeof(float)+band_lags_.capacity()*sizeof(std::size_t);
}
bool PreparedBandRhythmTransform::storage_bytes(std::size_t bands,BandRhythmTransformConfig config,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(config.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    std::size_t cells=0;const auto n=config.frame_size,bins=n/2+1;
    return bands && n>=2 && n<=INT_MAX && config.hop && config.hop<=INT_MAX && mul_bytes(bands,n,cells) &&
        PreparedFourierTempogram::storage_bytes(beat_window(n,config.hop),{},n,true,bytes,factory) && add_array(bytes,cells,sizeof(float)) &&
        add_array(bytes,bands,2*sizeof(float)) && add_array(bytes,bins,4*sizeof(float)) && add_array(bytes,n,sizeof(float));
}
std::size_t PreparedBandRhythmTransform::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+(derivative_.capacity()+current_.capacity()+previous_.capacity()+complex_.capacity()+magnitude_.capacity()+power_.capacity())*sizeof(float);
}
bool PreparedActivityFluctuation::geometry(std::size_t maximum,ActivityFluctuationConfig c,std::size_t& frame,std::size_t& scales) noexcept {
    const float bound=std::nextafter(float(INT_MAX),0.f),size=.01f*c.sample_rate;
    if(!maximum || maximum>INT_MAX || !std::isfinite(size) || size<2 || size>bound || !std::isfinite(c.minimum_tau_ms) ||
       !std::isfinite(c.maximum_tau_ms) || !std::isfinite(c.tau_multiplier) || c.minimum_tau_ms<20 || c.maximum_tau_ms<c.minimum_tau_ms || c.maximum_tau_ms/10>bound || c.tau_multiplier<=1)return false;
    frame=std::size_t(size);scales=0;
    for(float tau=c.minimum_tau_ms;tau<=c.maximum_tau_ms;){
        if(scales==SIZE_MAX)return false;++scales;const float next=tau*c.tau_multiplier;if(next==tau)return false;tau=next;
    }
    return scales>0;
}
bool PreparedActivityFluctuation::storage_bytes(std::size_t maximum,ActivityFluctuationConfig c,std::size_t& bytes) noexcept {
    std::size_t frame=0,scales=0;
    return geometry(maximum,c,frame,scales) && PreparedDetrendedFluctuation::required_workspace_bytes(scales,bytes) &&
        add_array(bytes,scales,2*sizeof(std::size_t)) && add_array(bytes,maximum/frame,sizeof(float));
}
}
