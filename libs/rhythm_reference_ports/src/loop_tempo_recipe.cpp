// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia LoopBpmEstimator at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/loop_tempo_recipe.h>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedLoopTempoRecipe::prepare(std::size_t maximum,float threshold,dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) {
    if(!std::isfinite(threshold) || threshold<0 || threshold>1)return false;
    std::size_t bound=0;if(!storage_bytes(maximum,threshold,bound,precision,factory))return false;
    PreparedLoopTempoRecipe next;
    PulseTempoRecipeConfig config;config.window_precision=precision;
    if(!next.tempo_.prepare(maximum,config,{},factory) || !next.confidence_.prepare())return false;
    next.threshold_=threshold;if(next.workspace_bytes()>bound)return false;*this=std::move(next);return true;
}
void PreparedLoopTempoRecipe::reset() noexcept {
    tempo_.reset();bpm_=0;confidence_result_={};
}
bool PreparedLoopTempoRecipe::process(std::span<const float> pcm) noexcept {
    bpm_=0;confidence_result_={};
    if(!tempo_.process(pcm))return false;
    const float rounded=std::round(tempo_.result().bpm);
    return confidence_.process(pcm,rounded,confidence_result_) &&
        select_loop_tempo(rounded,confidence_result_.confidence,threshold_,bpm_);
}
}
