#pragma once
#include "python_export_next.h"
namespace dsp_kernels {struct PcmSpectralNoveltyConfig;}
namespace originals {
dsp_kernels::PcmSpectralNoveltyConfig recipe_pcm_novelty(const Request&);
bool remaining(const Request&,const Matrix&,Result&);
bool remaining_models(const Request&,const Matrix&,Result&);
bool remaining_tempo(const Request&,const Matrix&,Result&);
bool remaining_pcm(const Request&,const Matrix&,Result&);
}
