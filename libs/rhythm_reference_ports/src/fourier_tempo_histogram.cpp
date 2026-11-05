// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BpmHistogram at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/fourier_tempo_histogram.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedFourierTempoHistogram::prepare_stages(std::size_t frames,std::size_t bins,std::span<const float> window) {
    const auto& p=config_.path;
    FourierTempoCandidateConfig candidates{p.frequency_step,p.minimum_bpm,p.maximum_bpm,config_.maximum_peaks};
    WeightedTempoHistogramConfig ranking{p.minimum_bpm,p.maximum_bpm,p.tolerance_percent,config_.weight_by_magnitude};
    PhaseTempoPulseConfig pulse{p.frame_rate,p.frequency_step,p.hop};
    return per_frame_.prepare(bins,candidates) && ranking_.prepare(frames*config_.maximum_peaks,frames,ranking) &&
        path_.prepare(frames,p) && pulse_.prepare(frames,bins,window,pulse);
}
void PreparedFourierTempoHistogram::allocate(std::size_t frames,std::size_t bins) {
    magnitude_.resize(bins);tempogram_.resize(frames*tempo_bins_);
    peak_offsets_.resize(frames+1);raw_peaks_.resize(frames*config_.maximum_peaks);
    observations_.resize(raw_peaks_.size());
}
bool PreparedFourierTempoHistogram::configure(std::size_t frames,std::size_t bins,std::span<const float> window,FourierTempoHistogramConfig config) {
    if(!frames || frames==SIZE_MAX || !config.maximum_peaks || frames>SIZE_MAX/config.maximum_peaks)return false;
    config.path.minimum_bpm=std::floor(config.path.minimum_bpm);
    config.path.maximum_bpm=std::ceil(config.path.maximum_bpm);config_=config;
    if(!prepare_stages(frames,bins,window))return false;
    tempo_bins_=per_frame_.tempo_row().size();
    if(!tempo_bins_ || frames>SIZE_MAX/tempo_bins_ || frames>std::size_t(std::numeric_limits<std::ptrdiff_t>::max()))return false;
    maximum_frames_=frames;allocate(frames,bins);return true;
}
bool PreparedFourierTempoHistogram::prepare(std::size_t frames,std::size_t bins,std::span<const float> window,FourierTempoHistogramConfig config) {
    std::size_t bound=0;if(!storage_bytes(frames,bins,window,config,bound))return false;
    PreparedFourierTempoHistogram next;
    if(!next.configure(frames,bins,window,config) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedFourierTempoHistogram::clear_outputs() noexcept {
    visible_frames_=0;result_bpm_=0;status_=FrameTempoPathStatus::Unprocessed;pulse_.reset();
}
void PreparedFourierTempoHistogram::reset(bool reset_reference) noexcept {
    clear_outputs();path_.reset(reset_reference);input_frames_=0;peak_count_=0;observation_count_=0;
}
bool PreparedFourierTempoHistogram::collect_frame(dsp_kernels::FloatMatrixSource source,std::size_t index) noexcept {
    if(!source.read(source.context,index,magnitude_) || !per_frame_.process(magnitude_))return false;
    peak_offsets_[index]=peak_count_;
    const auto raw=per_frame_.spectral_peaks(),observations=per_frame_.bpm_observations();
    std::copy(raw.begin(),raw.end(),raw_peaks_.begin()+std::ptrdiff_t(peak_count_));
    peak_count_+=raw.size();
    std::copy(observations.begin(),observations.end(),observations_.begin()+std::ptrdiff_t(observation_count_));
    observation_count_+=observations.size();
    const auto tempo=per_frame_.tempo_row();
    std::copy(tempo.begin(),tempo.end(),tempogram_.begin()+std::ptrdiff_t(index*tempo_bins_));
    return true;
}
bool PreparedFourierTempoHistogram::collect(dsp_kernels::FloatMatrixSource source,std::size_t frames) noexcept {
    peak_count_=0;observation_count_=0;input_frames_=frames;
    for(std::size_t i=0;i<frames;++i)if(!collect_frame(source,i))return false;
    peak_offsets_[frames]=peak_count_;return true;
}
bool PreparedFourierTempoHistogram::read_observation(const void* context,std::size_t index,std::span<float> row) noexcept {
    const auto& self=*static_cast<const PreparedFourierTempoHistogram*>(context);
    if(row.size()!=2 || index>=self.observation_count_)return false;
    const auto peak=self.observations_[index];row[0]=peak.position;row[1]=peak.value;return true;
}
bool PreparedFourierTempoHistogram::read_tempogram(const void* context,std::size_t index,std::span<float> row) noexcept {
    const auto& self=*static_cast<const PreparedFourierTempoHistogram*>(context);
    if(row.size()!=self.tempo_bins_ || index>=self.input_frames_)return false;
    const auto values=std::span(self.tempogram_).subspan(index*self.tempo_bins_,self.tempo_bins_);
    std::copy(values.begin(),values.end(),row.begin());return true;
}
std::span<const InterpolatedPeak> PreparedFourierTempoHistogram::read_peaks(const void* context,std::size_t index) noexcept {
    const auto& self=*static_cast<const PreparedFourierTempoHistogram*>(context);
    if(index>=self.input_frames_)return {};
    const auto first=self.peak_offsets_[index],last=self.peak_offsets_[index+1];
    return std::span(self.raw_peaks_).subspan(first,last-first);
}
bool PreparedFourierTempoHistogram::finish(dsp_kernels::FloatMatrixSource phases,std::size_t frames) noexcept {
    const dsp_kernels::FloatMatrixSource observations{this,read_observation},tempogram{this,read_tempogram};
    if(!ranking_.process(observations,observation_count_,tempogram,frames))return false;
    if(ranking_.silent()){status_=FrameTempoPathStatus::NoReference;return true;}
    const auto candidates=ranking_.candidates();
    const float main=path_.mean_bpm()>0?path_.mean_bpm():(candidates.empty()?0:candidates.front().position);
    if(!path_.process({this,read_peaks},frames,main))return false;
    status_=path_.status();
    if(status_!=FrameTempoPathStatus::Ready)return true;
    if(!pulse_.process(phases,path_.values()))return false;
    result_bpm_=path_.mean_bpm();visible_frames_=frames;return true;
}
bool PreparedFourierTempoHistogram::process(dsp_kernels::FloatMatrixSource magnitude,dsp_kernels::FloatMatrixSource phase,std::size_t frames) noexcept {
    clear_outputs();
    if(!maximum_frames_ || frames>maximum_frames_ || (frames && !magnitude.read))return false;
    if(!collect(magnitude,frames) || !finish(phase,frames)){clear_outputs();return false;}
    return true;
}
std::span<const InterpolatedPeak> PreparedFourierTempoHistogram::candidates() const noexcept {
    return visible_frames_?ranking_.candidates():std::span<const InterpolatedPeak>{};
}
std::span<const float> PreparedFourierTempoHistogram::frame_bpms() const noexcept {
    return visible_frames_?path_.values():std::span<const float>{};
}
std::span<const float> PreparedFourierTempoHistogram::tempogram_frame(std::size_t index) const noexcept {
    if(index>=visible_frames_)return {};
    return std::span(tempogram_).subspan(index*tempo_bins_,tempo_bins_);
}
std::span<const float> PreparedFourierTempoHistogram::analysis_tempogram_frame(std::size_t index) const noexcept {
    if(index>=input_frames_)return {};
    return std::span(tempogram_).subspan(index*tempo_bins_,tempo_bins_);
}
std::span<const float> PreparedFourierTempoHistogram::pulse() const noexcept {
    return visible_frames_?pulse_.pulse():std::span<const float>{};
}
std::span<const InterpolatedPeak> PreparedFourierTempoHistogram::ticks() const noexcept {
    return visible_frames_?pulse_.ticks():std::span<const InterpolatedPeak>{};
}
}
