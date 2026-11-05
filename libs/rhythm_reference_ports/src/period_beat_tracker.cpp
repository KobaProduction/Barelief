// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of TempoTapDegara::compute/computeBeatPeriodsDavies at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 C++ decomposition Copyright (C) 2026 Koba Production.
// AGPL version 3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/period_beat_tracker.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace rhythm_reference_ports {
namespace {
struct FrameRows {
    std::span<const float> values;
    const dsp_kernels::PreparedTemporalWindow* window;
    static bool read(const void* context,std::size_t frame,std::span<float> row) noexcept {
        const auto& source=*static_cast<const FrameRows*>(context);
        return source.window->read(source.values,frame,row);
    }
};
}
bool PreparedPeriodBeatTracker::prepare(std::size_t maximum,PeriodBeatTrackingConfig config) {
    std::size_t admitted=0;if(!storage_bytes(maximum,config,admitted))return false;
    PreparedPeriodBeatTracker next;if(!next.configure(maximum,config))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedPeriodBeatTracker::configure(std::size_t maximum,PeriodBeatTrackingConfig config) {
    if(!maximum || !config.upsampling || config.upsampling>4 || maximum-1>(SIZE_MAX-1)/config.upsampling)return false;
    if(!std::isfinite(config.frame_rate) || config.frame_rate<=0)return false;
    maximum_samples_=maximum;config_=config;rate_=config.frame_rate*float(config.upsampling);
    if(!prepare_expansion() || !prepare_window() || !prepare_estimators())return false;
    allocate();return true;
}
bool PreparedPeriodBeatTracker::prepare_expansion() {
    std::size_t maximum=0;
    if(!dsp_kernels::endpoint_expansion_size(maximum_samples_,config_.upsampling,maximum))return false;
    return expansion_.prepare_window({maximum_samples_,maximum,1,
        dsp_kernels::ResamplingWindowMode::EndpointLinear,config_.upsampling});
}
bool PreparedPeriodBeatTracker::prepare_window() {
    const float length=std::round(5.944308390022676f*rate_);
    if(!std::isfinite(length) || length<16 || length>=float(UINT32_MAX))return false;
    dsp_kernels::TemporalWindowConfig config;config.length=std::size_t(length);config.hop=config.length/4;
    config.function=dsp_kernels::WindowFunction::Rectangular;config.periodic=false;config.center=false;
    config.tail=dsp_kernels::TemporalTail::CoverEnd;return window_.prepare(config);
}
bool PreparedPeriodBeatTracker::prepare_estimators() {
    const auto maximum=output_samples(maximum_samples_),blocks=window_.frames(maximum);
    PeriodPathConfig periods;periods.frame_rate=rate_;periods.min_bpm=config_.min_bpm;periods.max_bpm=config_.max_bpm;
    periods.frame_length=window_.config().length;periods.hop=window_.config().hop;periods.noise=config_.noise;
    const RenewalBeatConfig decoder{.frame_rate=config_.frame_rate,
        .maximum_period_s=float(periods.hop)/rate_,.upsampling=config_.upsampling};
    return prepare_threshold(maximum) &&
        period_.prepare(blocks,periods) && decoder_.prepare(maximum,blocks,decoder);
}
bool PreparedPeriodBeatTracker::prepare_threshold(std::size_t maximum) {
    const auto half=std::size_t(std::floor(.1f*rate_));
    return threshold_.prepare(maximum,half) && average_.prepare(2*half+1);
}
void PreparedPeriodBeatTracker::allocate() {
    const auto maximum=output_samples(maximum_samples_),blocks=window_.frames(maximum);
    normalized_.resize(maximum_samples_);expanded_.resize(maximum);thresholded_.resize(maximum);
    periods_.resize(blocks);ends_.resize(blocks);mask_.resize(maximum);
}
std::size_t PreparedPeriodBeatTracker::output_samples(std::size_t samples) const noexcept {
    return expansion_.window_output_samples(samples);
}
bool PreparedPeriodBeatTracker::normalize_expand(std::span<const float> onset) noexcept {
    const float maximum=*std::max_element(onset.begin(),onset.end());
    for(std::size_t i=0;i<onset.size();++i)normalized_[i]=maximum>0?onset[i]/maximum:0;
    const auto expected=output_samples(onset.size());std::size_t written=0;
    return expansion_.process_window_prefix(std::span(normalized_).first(onset.size()),onset.size(),onset.size(),
        std::span(expanded_).first(expected),expected,written) && written==expected;
}
bool PreparedPeriodBeatTracker::estimate_periods(std::size_t samples,std::size_t blocks) noexcept {
    average_.reset();
    if(!threshold_.process(std::span(expanded_).first(samples),std::span(thresholded_).first(samples),average_))return false;
    const FrameRows rows{std::span(thresholded_).first(samples),&window_};
    return period_.process({&rows,FrameRows::read},blocks,std::span(periods_).first(blocks),std::span(ends_).first(blocks),average_);
}
bool PreparedPeriodBeatTracker::process(std::span<const float> onset,std::span<std::uint8_t> output) noexcept {
    result_samples_=0;result_blocks_=0;
    if(!maximum_samples_ || onset.size()>maximum_samples_ || output.size()!=output_samples(onset.size()))return false;
    if(!std::all_of(onset.begin(),onset.end(),[](float value){return std::isfinite(value) && value>=0;}))return false;
    if(onset.empty())return true;
    const auto samples=output_samples(onset.size()),blocks=window_.frames(samples);
    if(!normalize_expand(onset) || !estimate_periods(samples,blocks))return false;
    if(!decoder_.process(std::span(expanded_).first(samples),std::span(periods_).first(blocks),
                         std::span(ends_).first(blocks),std::span(mask_).first(samples)))return false;
    std::copy_n(mask_.begin(),samples,output.begin());result_samples_=samples;result_blocks_=blocks;return true;
}
bool PreparedPeriodBeatTracker::beat_times(std::span<float> output,std::size_t& count) const noexcept {
    count=0;
    if(!maximum_samples_)return false;
    const auto beats=std::size_t(std::count(mask_.begin(),mask_.begin()+std::ptrdiff_t(result_samples_),std::uint8_t{1}));
    if(output.size()<beats)return false;
    std::size_t next=0;
    for(std::size_t i=0;i<result_samples_;++i)if(mask_[i])output[next++]=float(i)*resolution_seconds();
    count=next;return true;
}
}
