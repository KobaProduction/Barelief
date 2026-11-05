#pragma once
#include "python_export.h"
#include <dsp_kernels/envelope_tempo_ratios.h>
#include <dsp_kernels/envelope_beat_tracking.h>

namespace originals {
// New finite packet. Existing driver dispatch is wired only at source fan-in.
bool compositions(const Request&,const Matrix&,Result&);
bool pcm_compositions(const Request&,const Matrix&,Result&);
bool patterns(const Request&,const Matrix&,Result&);
dsp_kernels::EnvelopeTempoConfig recipe_tempo(const Request&);
dsp_kernels::TempogramSequenceConfig recipe_tempogram(const Request&);
dsp_kernels::EnvelopeTempoRatiosConfig recipe_ratios(const Request&);
dsp_kernels::PcmOnsetStrengthConfig recipe_onset(const Request&,bool median=false);
dsp_kernels::EnvelopeBeatTrackingConfig recipe_beats(const Request&);
std::optional<std::span<const float>> recipe_bpm(const Request&,const float&);
Matrix sidecar(const Request&,std::string_view,std::size_t,std::size_t);
void recipe_beat_outputs(const dsp_kernels::BeatTrackingSequenceResult&,Result&);
}
