// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/pulse_tempo_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <bit>
#include <cmath>
#include <limits>
#include <climits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
namespace {
bool append(std::size_t& bytes,std::size_t value) noexcept {return prepared_storage::add_bytes(bytes,value);}
TemporalWindowConfig pulse_spectrum_config(PulseTempoRecipeConfig c) noexcept {
    TemporalWindowConfig spectrum;spectrum.length=c.frame_size;spectrum.hop=c.audio_hop;spectrum.center=false;spectrum.periodic=false;spectrum.function=WindowFunction::Hamming;
    return spectrum;
}
TemporalWindowConfig pulse_onset_config(PulseTempoRecipeConfig c) noexcept {
    TemporalWindowConfig onset;onset.length=c.onset_window;onset.hop=c.onset_hop;onset.center=false;onset.function=WindowFunction::Rectangular;onset.tail=TemporalTail::CoverEnd;
    return onset;
}
}
bool PreparedPulseOnset::storage_bytes(std::size_t bins,std::size_t fft,std::size_t& out) noexcept {
    if(fft<2 || bins!=fft/2+1)return false;
    std::size_t bytes=0,nested=0;
    if(!PreparedSpectralDescriptor::storage_bytes(bins,bytes) || !PreparedRecursiveFilter::storage_bytes(15,15,{},nested) ||
       !append(bytes,nested) || !prepared_storage::add_array(bytes,bins,2*sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedPulseOnset::workspace_bytes() const noexcept {
    return difference_.workspace_bytes()+filter_.storage_bytes()+(scaled_.capacity()+compressed_.capacity())*sizeof(float);
}
bool PreparedPulseTrainScoring::storage_bytes(std::size_t lag,std::size_t candidates,std::size_t& out) noexcept {
    if(!lag || lag>SIZE_MAX/12 || !candidates)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,lag,sizeof(float)) || !prepared_storage::add_array(bytes,candidates,3*sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedPulseTrainScoring::workspace_bytes() const noexcept {
    return (phases_.capacity()+magnitudes_.capacity()+variances_.capacity()+scores_.capacity())*sizeof(float);
}
bool PreparedPulseCorrelation::storage_bytes(std::size_t frames,float rate,float minimum,float maximum,std::size_t& out,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(frames<2 || frames>SIZE_MAX/4 || !std::isfinite(rate) || rate<=0 || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum<=0 || minimum>=maximum)return false;
    const auto length=std::bit_ceil(2*frames);
    SpectralAutocorrelationConfig acf{length,float(length),.5f,float(length)};
    InterpolatedPeakConfig peaks;peaks.range=float(frames-1);peaks.minimum=std::trunc(rate*60.f/maximum);
    peaks.maximum=std::trunc(rate*60.f/minimum);peaks.maximum_peaks=10;peaks.ordering=PeakOrdering::Amplitude;
    std::size_t bytes=0,nested=0;
    if(!PreparedSpectralAutocorrelation::storage_bytes(frames,acf,bytes,factory) || !PreparedInterpolatedPeaks::storage_bytes(frames,peaks,nested) || !append(bytes,nested) ||
       !PreparedPulseTrainScoring::storage_bytes(frames,10,nested) || !append(bytes,nested) ||
       !prepared_storage::add_array(bytes,frames,2*sizeof(float)) || !prepared_storage::add_array(bytes,10,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedPulseCorrelation::workspace_bytes() const noexcept {
    return correlation_.workspace_bytes()+peaks_.workspace_bytes()+scoring_.workspace_bytes()+
        (correlation_values_.capacity()+enhanced_.capacity()+candidate_lags_.capacity())*sizeof(float);
}
bool PreparedPulseTempoRecipe::storage_bytes(std::size_t maximum,PulseTempoRecipeConfig c,TempoOctaveClassifier model,std::size_t& out,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(c.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    const auto limit=std::size_t(INT_MAX);
    if(!maximum || maximum>limit || c.frame_size<2 || c.frame_size>limit || !c.audio_hop || c.audio_hop>limit ||
       c.onset_window<2 || c.onset_window>limit || !c.onset_hop || c.onset_hop>limit || c.sample_rate<=0 || c.minimum_bpm<=0 || c.minimum_bpm>=c.maximum_bpm)return false;
    const auto spectrum=pulse_spectrum_config(c);
    const auto onset=pulse_onset_config(c);
    const float rate=float(c.sample_rate)/float(c.audio_hop);std::size_t bytes=0,nested=0;
    if(!PreparedFourierTempogram::storage_bytes(spectrum,{},c.frame_size,false,bytes,factory) || !PreparedTemporalWindow::storage_bytes(onset,{},nested) || !append(bytes,nested) ||
       !PreparedPulseOnset::storage_bytes(c.frame_size/2+1,c.frame_size,nested) || !append(bytes,nested) || !ScalarHistory::storage_bytes(c.onset_window,nested) || !append(bytes,nested))return false;
    if(!PreparedPulseCorrelation::storage_bytes(c.onset_window,rate,float(c.minimum_bpm),float(c.maximum_bpm),nested,factory) || !append(bytes,nested) ||
       !PreparedLagTempoConsensus::storage_bytes(rate,model,nested) || !append(bytes,nested) ||
       !prepared_storage::add_array(bytes,c.frame_size/2+1,3*sizeof(float)) || !prepared_storage::add_array(bytes,c.onset_window,sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.frame_size,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedPulseTempoRecipe::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+onset_window_.workspace_bytes()+onset_.workspace_bytes()+correlation_.workspace_bytes()+
        consensus_.workspace_bytes()+history_.workspace_bytes()+(complex_.capacity()+magnitude_.capacity()+tail_.capacity())*sizeof(float);
}
}
