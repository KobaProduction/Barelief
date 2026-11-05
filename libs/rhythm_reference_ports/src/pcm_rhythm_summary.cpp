// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia RhythmExtractor2013/RhythmDescriptors at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/pcm_rhythm_summary.h>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/resampling_window.h>
#include <algorithm>

namespace rhythm_reference_ports {
bool PreparedPcmRhythmSummary::prepare(std::size_t maximum,PcmBeatRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedPcmRhythmSummary next;std::size_t bytes=0;if(!storage_bytes(maximum,config,bytes,factory))return false;
    if(!next.tracker_.prepare(maximum,config,factory) ||
       !next.summary_.prepare(next.tracker_.maximum_tick_count(),60.f*44100.f/512.f) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmRhythmSummary::process(std::span<const float> pcm,PcmBeatStageOutputs output) noexcept {
    ready_=false;
    if(!tracker_.process(pcm,output) || !summary_.process(tracker_.ticks(),tracker_.confidence()))return false;
    ready_=true;return true;
}
bool PreparedPcmRhythmSummary::maximum_tick_count(std::size_t maximum,PcmBeatRecipeConfig config,std::size_t& output) noexcept {
    dsp_kernels::TemporalWindowConfig window;window.length=2048;window.hop=1024;window.center=false;window.tail=dsp_kernels::TemporalTail::CoverEnd;
    std::size_t frames=0,count=0,more=0;
    if(!dsp_kernels::PreparedTemporalWindow::frame_count(window,maximum,frames) || !frames ||
       !dsp_kernels::endpoint_expansion_size(frames,2,count))return false;
    if(config.method==PcmBeatMethod::MultiFeature){
        window.hop=512;if(!dsp_kernels::PreparedTemporalWindow::frame_count(window,maximum,more))return false;
        count=std::max(count,more);
    }
    output=count;return true;
}
bool PreparedPcmRhythmSummary::storage_bytes(std::size_t maximum,PcmBeatRecipeConfig config,std::size_t& output,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    std::size_t bytes=0,ticks=0,nested=0;
    if(!PreparedPcmBeatRecipe::storage_bytes(maximum,config,bytes,factory) || !maximum_tick_count(maximum,config,ticks) ||
       !PreparedBeatIntervalSummary::storage_bytes(ticks,60.f*44100.f/512.f,nested) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested))return false;
    output=bytes;return true;
}
bool PreparedPcmRhythmSummary::histogram_descriptors(BeatIntervalHistogram& output) const noexcept {
    return ready_ && summary_.histogram_descriptors(output);
}
}
