// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/band_novelty.h>
#include <rhythm_reference_ports/fixed_tempo_candidates.h>
#include <rhythm_reference_ports/fourier_tempo_candidates.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
using namespace dsp_kernels;
namespace {
InterpolatedPeakConfig fixed_tempo_peak_config(std::size_t frames,float lag) noexcept {
    InterpolatedPeakConfig peak;peak.range=float(frames-1);peak.maximum=peak.range;
    peak.minimum=lag;peak.threshold=0;peak.maximum_peaks=frames;return peak;
}
}
bool PreparedBandNovelty::storage_bytes(std::size_t frames,std::size_t bands,BandNoveltyConfig c,std::span<const float> weights,std::size_t& out) noexcept {
    if(!frames || !bands || bands>46340 || frames>std::size_t(PTRDIFF_MAX)/bands || !std::isfinite(c.frame_rate) || c.frame_rate<1 || unsigned(c.weighting)>unsigned(NoveltyWeight::Hybrid))return false;
    const double smoothing=.1*double(c.frame_rate);
    if(smoothing<1 || smoothing>double(PTRDIFF_MAX/2))return false;
    if(c.weighting==NoveltyWeight::Supplied && (weights.size()!=bands || !std::all_of(weights.begin(),weights.end(),[](float x){return std::isfinite(x);})))return false;
    auto mean=std::size_t(smoothing);mean+=mean%2;std::size_t bytes=0;
    if(!PreparedCausalAverage::storage_bytes(mean,bytes) || !prepared_storage::add_array(bytes,bands,((c.weighting==NoveltyWeight::Hybrid?4:1)+3)*sizeof(float)) ||
       !prepared_storage::add_array(bytes,(frames-1)*bands,sizeof(float)) || !prepared_storage::add_array(bytes,frames-1,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedBandNovelty::workspace_bytes() const noexcept {
    return average_.workspace_bytes()+(row_.capacity()+current_.capacity()+previous_.capacity()+derived_.capacity()+weights_.capacity()+mixed_.capacity())*sizeof(float);
}
bool PreparedFixedTempoCandidates::storage_bytes(std::size_t frames,FixedTempoCandidateConfig c,std::size_t& out,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(frames<3 || !std::isfinite(c.sample_rate) || c.sample_rate<1 || !std::isfinite(60.f*c.sample_rate) || !c.hop_samples ||
       !std::isfinite(c.minimum_bpm) || !std::isfinite(c.maximum_bpm) || c.minimum_bpm<=0 || c.minimum_bpm>=c.maximum_bpm ||
       !std::isfinite(c.tolerance_percent) || c.tolerance_percent<=0 || c.tolerance_percent>100)return false;
    const float lag=std::round(60.f*c.sample_rate/c.maximum_bpm/float(c.hop_samples));
    if(!std::isfinite(lag) || lag<1 || lag>=float(frames-1))return false;
    SpectralAutocorrelationConfig acf;acf.normalization=CorrelationNormalization::Unbiased;
    auto peak=fixed_tempo_peak_config(frames,lag);
    std::size_t bytes=0,nested=0;
    if(!PreparedSpectralAutocorrelation::storage_bytes(frames,acf,bytes,factory) || !PreparedCausalAverage::storage_bytes(std::size_t(lag),nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedInterpolatedPeaks::storage_bytes(frames,peak,nested) || !prepared_storage::add_bytes(bytes,nested))return false;
    peak.range=peak.maximum=float(frames);
    if(!PreparedInterpolatedPeaks::storage_bytes(frames,peak,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedTempoDistributionPeaks::storage_bytes(nested) || !prepared_storage::add_bytes(bytes,nested) || !prepared_storage::add_array(bytes,frames,3*sizeof(float)+sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedFixedTempoCandidates::workspace_bytes() const noexcept {
    return correlation_.workspace_bytes()+average_.workspace_bytes()+initial_.workspace_bytes()+iteration_.workspace_bytes()+distribution_.workspace_bytes()+
        (correlation_values_.capacity()+smoothed_.capacity()+scratch_.capacity())*sizeof(float)+selected_.capacity()*sizeof(InterpolatedPeak);
}
bool PreparedFourierTempoCandidates::storage_bytes(std::size_t bins,FourierTempoCandidateConfig c,std::size_t& out) noexcept {
    if(bins<2 || !c.maximum_peaks || c.maximum_peaks==SIZE_MAX || !std::isfinite(c.frequency_step) || c.frequency_step<=0 || !std::isfinite(60.f*c.frequency_step) ||
       !std::isfinite(c.minimum_bpm) || !std::isfinite(c.maximum_bpm) || c.minimum_bpm<0 || c.maximum_bpm<=c.minimum_bpm || double(c.maximum_bpm)>double(SIZE_MAX/sizeof(float))-2)return false;
    c.minimum_bpm=std::floor(c.minimum_bpm);c.maximum_bpm=std::ceil(c.maximum_bpm);
    InterpolatedPeakConfig peak;peak.range=float(bins-1);peak.minimum=c.minimum_bpm/(60.f*c.frequency_step);peak.maximum=c.maximum_bpm/(60.f*c.frequency_step);
    peak.threshold=0;peak.maximum_peaks=c.maximum_peaks;peak.ordering=PeakOrdering::Amplitude;std::size_t bytes=0;
    if(!PreparedInterpolatedPeaks::storage_bytes(bins,peak,bytes) || !prepared_storage::add_array(bytes,bins,sizeof(float)) ||
       !prepared_storage::add_array(bytes,std::size_t(c.maximum_bpm)+1,sizeof(float)) || !prepared_storage::add_array(bytes,c.maximum_peaks+1,sizeof(InterpolatedPeak)))return false;
    out=bytes;return true;
}
std::size_t PreparedFourierTempoCandidates::workspace_bytes() const noexcept {
    return detector_.workspace_bytes()+(scratch_.capacity()+tempo_.capacity())*sizeof(float)+observations_.capacity()*sizeof(InterpolatedPeak);
}
}
