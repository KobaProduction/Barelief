// SPDX-License-Identifier: GPL-3.0-or-later
// BTrack.cpp, adamstark/BTrack@9d6127618a5679e9caa74c594b88f1d74f0e035f.
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/pcm_predictive_beat_tracker.h>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
bool PreparedPcmPredictiveBeatTracker::resample(void* context,std::span<const float> input,std::span<float> output) noexcept {
    auto& resampler=*static_cast<dsp_kernels::PreparedResampler*>(context);
    std::size_t written=0;
    return resampler.process_window(input,input.size(),output,output.size(),written) && written==output.size();
}
OnsetWindowResamplingPort PreparedPcmPredictiveBeatTracker::resampling_port(dsp_kernels::PreparedResampler& resampler) const noexcept {
    return {&resampler,&PreparedPcmPredictiveBeatTracker::resample};
}
std::unique_ptr<dsp_kernels::PreparedResampler> PreparedPcmPredictiveBeatTracker::prepare_resampler(
    const dsp_kernels::ResamplerBackendFactory& factory,std::uint32_t hop,dsp_kernels::NumericalPrecision precision) {
    if(!hop || hop>262144)return {};
    auto result=std::make_unique<dsp_kernels::PreparedResampler>();
    if(!result->prepare_window({std::size_t{262144}/hop,512,1,dsp_kernels::ResamplingWindowMode::SincBestQuality,1,precision},&factory))return {};
    return result;
}
bool PreparedPcmPredictiveBeatTracker::prepare(const dsp_kernels::ResamplerBackendFactory& factory,PcmPredictiveOnsetConfig c,
                                             const dsp_kernels::FourierBackendFactory* fft_factory) {
    std::size_t bytes=0;if(!storage_bytes(factory,c,bytes,fft_factory))return false;
    PreparedPcmPredictiveBeatTracker next;next.factory_=&factory;
    next.resampler_=prepare_resampler(factory,c.hop_samples,c.precision);
    if(!next.resampler_ || !next.frontend_.prepare(c,fft_factory) ||
        !next.tracker_.prepare(c.hop_samples,next.resampling_port(*next.resampler_),c.precision,fft_factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmPredictiveBeatTracker::storage_bytes(const dsp_kernels::ResamplerBackendFactory& factory,PcmPredictiveOnsetConfig config,
                                                   std::size_t& output,const dsp_kernels::FourierBackendFactory* fft_factory) noexcept {
    if(!config.hop_samples || config.hop_samples>262144)return false;
    std::size_t bytes=0,nested=0;dsp_kernels::ResamplerResources resources;
    if(!PreparedPcmPredictiveOnset::storage_bytes(config,bytes,fft_factory) ||
       !PreparedPredictiveBeatTracker::storage_bytes(config.hop_samples,nested,config.precision,fft_factory) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested) ||
       !dsp_kernels::PreparedResampler::window_resource_requirements({std::size_t{262144}/config.hop_samples,512,1,
           dsp_kernels::ResamplingWindowMode::SincBestQuality,1,config.precision},&factory,resources) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,resources.owned_bytes))return false;
    output=bytes;return true;
}
std::size_t PreparedPcmPredictiveBeatTracker::workspace_bytes() const noexcept {
    return frontend_.workspace_bytes()+tracker_.workspace_bytes()+resampler_resources().owned_bytes;
}
std::unique_ptr<PreparedPcmPredictiveBeatTracker> PreparedPcmPredictiveBeatTracker::clone() const {
    if(!factory_ || !resampler_ || !resampler_->window_config())return {};
    auto next=std::make_unique<PreparedPcmPredictiveBeatTracker>();next->factory_=factory_;
    next->frontend_=frontend_;next->tracker_=tracker_;next->resampler_=prepare_resampler(*factory_,frontend_.config().hop_samples,frontend_.config().precision);
    if(!next->resampler_ || !next->resampler_->copy_state_from(*resampler_) ||
       !next->tracker_.set_resampling_port(next->resampling_port(*next->resampler_)))return {};
    return next;
}
bool PreparedPcmPredictiveBeatTracker::process_onset(float onset,PcmPredictiveBeatObservation& output) noexcept {
    if(!resampler_)return false;
    BeatPredictionObservation prediction;
    const bool reference=frontend_.config().precision==dsp_kernels::NumericalPrecision::DesktopReference;
    if(!(reference?tracker_.process_reference(onset,prediction):tracker_.process(onset,prediction)))return false;
    output={onset,tracker_.bpm(),prediction};return true;
}
bool PreparedPcmPredictiveBeatTracker::process_hop(std::span<const float> pcm,PcmPredictiveBeatObservation& output) noexcept {
    float onset=0;
    if(!resampler_ || !frontend_.process_hop(pcm,onset))return false;
    if(frontend_.config().precision!=dsp_kernels::NumericalPrecision::DesktopReference)return process_onset(onset,output);
    BeatPredictionObservation prediction;
    if(!tracker_.process_reference(frontend_.reference_stage().value(),prediction))return false;
    output={onset,tracker_.bpm(),prediction};return true;
}
bool PreparedPcmPredictiveBeatTracker::update_hop_and_frame(std::uint32_t hop,std::uint32_t frame,
                                                          const dsp_kernels::FourierBackendFactory* fft_factory) {
    if(!factory_ || !resampler_)return false;
    auto c=frontend_.config();c.hop_samples=hop;c.frame_samples=frame;
    PreparedPcmPredictiveOnset frontend;
    auto resampler=prepare_resampler(*factory_,hop,c.precision);
    if(!resampler || !frontend.prepare(c,fft_factory))return false;
    auto tracker=tracker_;
    if(!tracker.set_hop_samples(hop) || !tracker.set_resampling_port(resampling_port(*resampler)))return false;
    frontend_=std::move(frontend);tracker_=std::move(tracker);resampler_=std::move(resampler);return true;
}
bool PreparedPcmPredictiveBeatTracker::restart() noexcept {
    if(!resampler_ || !tracker_.reset())return false;
    frontend_.restart();resampler_->reset();return true;
}
dsp_kernels::ResamplerResources PreparedPcmPredictiveBeatTracker::resampler_resources() const noexcept {
    return resampler_?resampler_->resources():dsp_kernels::ResamplerResources{};
}
}
