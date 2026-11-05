// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/novelty_tempo_histogram.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
namespace {
InterpolatedPeakConfig phase_pulse_peak_config(std::size_t samples) noexcept {
    InterpolatedPeakConfig peaks;peaks.range=float(samples-1);peaks.maximum=peaks.range;
    peaks.threshold=1e-4f;peaks.maximum_peaks=samples;return peaks;
}
FourierTempoHistogramConfig novelty_histogram_config(NoveltyTempoHistogramConfig c,std::size_t fft,std::size_t hop) noexcept {
    FourierTempoHistogramConfig histogram;histogram.maximum_peaks=c.maximum_peaks;histogram.weight_by_magnitude=c.weight_by_magnitude;
    histogram.path={c.frame_rate/float(fft),c.frame_rate,c.minimum_bpm,c.maximum_bpm,3,c.minimum_change_seconds,c.induced_bpm,hop,c.constant_tempo};
    return histogram;
}
}
bool PreparedWeightedTempoHistogram::storage_bytes(std::size_t observations,std::size_t frames,WeightedTempoHistogramConfig c,std::size_t& out) noexcept {
    if(!observations || !frames || !std::isfinite(c.minimum_bpm) || !std::isfinite(c.maximum_bpm) || c.minimum_bpm<0 || c.minimum_bpm>=c.maximum_bpm ||
       !std::isfinite(c.tolerance_percent) || c.tolerance_percent<0 || double(c.maximum_bpm)>=double(SIZE_MAX/sizeof(InterpolatedPeak))-2)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,std::size_t(std::ceil(c.maximum_bpm))+1,2*sizeof(float)+sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedWeightedTempoHistogram::workspace_bytes() const noexcept {return (histogram_.capacity()+frame_.capacity())*sizeof(float)+result_.capacity()*sizeof(InterpolatedPeak);}
bool PreparedFrameTempoPath::storage_bytes(std::size_t frames,FrameTempoPathConfig c,std::size_t& out) noexcept {
    if(!frames || !c.hop || !std::isfinite(c.frequency_step) || c.frequency_step<std::numeric_limits<float>::min() || !std::isfinite(c.frame_rate) || c.frame_rate<1 ||
       !std::isfinite(c.minimum_bpm) || c.minimum_bpm<0 || !std::isfinite(c.maximum_bpm) || c.maximum_bpm<std::numeric_limits<float>::min() || c.minimum_bpm>c.maximum_bpm ||
       !std::isfinite(c.tolerance_percent) || c.tolerance_percent<0 || !std::isfinite(c.minimum_change_seconds) || c.minimum_change_seconds<0 || !std::isfinite(c.induced_bpm) || c.induced_bpm<0)return false;
    const float stable=c.minimum_change_seconds*c.frame_rate/float(c.hop);
    if(!std::isfinite(stable) || stable>std::nextafter(float(INT_MAX),0.f) || !std::isfinite(c.frequency_step*60.f))return false;
    return prepared_storage::mul_bytes(frames,sizeof(float),out);
}
std::size_t PreparedFrameTempoPath::workspace_bytes() const noexcept {return bpms_.capacity()*sizeof(float);}
bool PreparedPhaseTempoPulse::storage_bytes(std::size_t frames,std::size_t bins,std::span<const float> window,PhaseTempoPulseConfig c,std::size_t& out) noexcept {
    return std::all_of(window.begin(),window.end(),[](float v){return std::isfinite(v);}) && storage_bytes(frames,bins,window.size(),c,out);
}
bool PreparedPhaseTempoPulse::storage_bytes(std::size_t frames,std::size_t bins,std::size_t window,PhaseTempoPulseConfig c,std::size_t& out) noexcept {
    if(!frames || bins<2 || !window || !c.hop || frames>SIZE_MAX/c.hop || !std::isfinite(c.frame_rate) || c.frame_rate<1 ||
       !std::isfinite(c.frequency_step) || c.frequency_step<=0 || !std::isfinite(c.frequency_step*60.f) || c.frequency_step*60.f<=0)return false;
    const auto samples=frames*c.hop;const float average=30.f*c.frame_rate;
    if(samples<2 || samples>std::size_t(PTRDIFF_MAX) || !std::isfinite(average) || average<1 || average>std::nextafter(float(INT_MAX),0.f))return false;
    const auto peaks=phase_pulse_peak_config(samples);
    std::size_t bytes=0,nested=0;
    if(!PreparedOverlapAdd::storage_bytes(frames,window,c.hop,OverlapNormalization::None,OverlapSampleSelection::Positive,bytes) ||
       !PreparedCausalAverage::storage_bytes(std::size_t(average),nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedInterpolatedPeaks::storage_bytes(samples,peaks,nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    if(!prepared_storage::add_array(bytes,bins,sizeof(float)) || !prepared_storage::add_array(bytes,window,sizeof(float)) ||
       !prepared_storage::add_array(bytes,samples,sizeof(float)+sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedPhaseTempoPulse::workspace_bytes() const noexcept {
    return overlap_.workspace_bytes()+average_.workspace_bytes()+detector_.workspace_bytes()+(phase_.capacity()+frame_.capacity()+pulse_.capacity())*sizeof(float)+ticks_.capacity()*sizeof(InterpolatedPeak);
}
bool PreparedFourierTempoHistogram::storage_bytes(std::size_t frames,std::size_t bins,std::span<const float> window,FourierTempoHistogramConfig c,std::size_t& out) noexcept {
    return std::all_of(window.begin(),window.end(),[](float v){return std::isfinite(v);}) && storage_bytes(frames,bins,window.size(),c,out);
}
bool PreparedFourierTempoHistogram::storage_bytes(std::size_t frames,std::size_t bins,std::size_t window,FourierTempoHistogramConfig c,std::size_t& out) noexcept {
    if(!frames || frames==SIZE_MAX || !c.maximum_peaks || frames>SIZE_MAX/c.maximum_peaks || frames>std::size_t(PTRDIFF_MAX))return false;
    auto& p=c.path;p.minimum_bpm=std::floor(p.minimum_bpm);p.maximum_bpm=std::ceil(p.maximum_bpm);
    const auto peaks=frames*c.maximum_peaks;std::size_t bytes=0,nested=0,tempo=0;
    if(!PreparedFourierTempoCandidates::storage_bytes(bins,{p.frequency_step,p.minimum_bpm,p.maximum_bpm,c.maximum_peaks},bytes) ||
       !PreparedWeightedTempoHistogram::storage_bytes(peaks,frames,{p.minimum_bpm,p.maximum_bpm,p.tolerance_percent,c.weight_by_magnitude},nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedFrameTempoPath::storage_bytes(frames,p,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedPhaseTempoPulse::storage_bytes(frames,bins,window,{p.frame_rate,p.frequency_step,p.hop},nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    if(!prepared_storage::mul_bytes(frames,std::size_t(p.maximum_bpm)+1,tempo) || !prepared_storage::add_array(bytes,bins,sizeof(float)) ||
       !prepared_storage::add_array(bytes,tempo,sizeof(float)) || !prepared_storage::add_array(bytes,frames+1,sizeof(std::size_t)) || !prepared_storage::add_array(bytes,peaks,2*sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedFourierTempoHistogram::workspace_bytes() const noexcept {
    return per_frame_.workspace_bytes()+ranking_.workspace_bytes()+path_.workspace_bytes()+pulse_.workspace_bytes()+
        (magnitude_.capacity()+tempogram_.capacity())*sizeof(float)+peak_offsets_.capacity()*sizeof(std::size_t)+(raw_peaks_.capacity()+observations_.capacity())*sizeof(InterpolatedPeak);
}
bool PreparedNoveltyTempoHistogram::dimensions(std::size_t maximum,NoveltyTempoHistogramConfig c,std::size_t& window,std::size_t& fft,std::size_t& hop,std::size_t& frames) noexcept {
    if(!maximum || maximum>std::size_t(PTRDIFF_MAX)/2)return false;
    PreparedNoveltyTempoHistogram metadata;TemporalWindowConfig frame;std::size_t padding=0;
    if(!metadata.geometry(c,frame,padding) || !PreparedTemporalWindow::frame_count(frame,maximum,frames))return false;
    window=frame.length;fft=frame.length+padding;hop=frame.hop;return true;
}
bool PreparedNoveltyTempoHistogram::storage_bytes(std::size_t maximum,NoveltyTempoHistogramConfig c,std::span<const float> window,std::size_t& out,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(c.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    if(!maximum || maximum>std::size_t(PTRDIFF_MAX)/2)return false;
    PreparedNoveltyTempoHistogram metadata;TemporalWindowConfig frame;std::size_t padding=0,frames=0,bytes=0,nested=0;
    if(!metadata.storage_geometry(maximum,c,window,frame,padding,frames))return false;
    const auto fft=frame.length+padding;
    const auto histogram=novelty_histogram_config(c,fft,frame.hop);
    if(!PreparedFourierTempogram::storage_bytes(frame,{},fft,true,bytes,factory) || !PreparedFourierTempoHistogram::storage_bytes(frames,fft/2+1,frame.length,histogram,nested) ||
       !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,frame.length,2*sizeof(float)) || !prepared_storage::add_array(bytes,fft/2+1,2*sizeof(float)))return false;
    out=bytes;return true;
}
bool PreparedNoveltyTempoHistogram::storage_geometry(std::size_t maximum,NoveltyTempoHistogramConfig c,std::span<const float> window,
    TemporalWindowConfig& frame,std::size_t& padding,std::size_t& frames) noexcept {
    if(!geometry(c,frame,padding) || (!window.empty() && window.size()!=frame.length) ||
       !std::all_of(window.begin(),window.end(),[](float v){return std::isfinite(v);}) || !PreparedTemporalWindow::frame_count(frame,maximum,frames))return false;
    std::size_t nested=0;
    return !window.empty() || spectral_window_storage_bytes(frame.length,{c.window},nested);
}
std::size_t PreparedNoveltyTempoHistogram::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+histogram_.workspace_bytes()+(analysis_window_.capacity()+synthesis_window_.capacity()+complex_.capacity())*sizeof(float);
}
}
