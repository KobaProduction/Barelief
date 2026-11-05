// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia RhythmTransform at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/band_rhythm_transform.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedBandRhythmTransform::prepare_spectrum(const dsp_kernels::FourierBackendFactory* factory,std::span<float> coefficients) {
    dsp_kernels::TemporalWindowConfig window;window.length=config_.frame_size;window.hop=config_.hop;window.center=false;
    SpectralWindowConfig shape;shape.precision=config_.window_precision;shape.shape=SpectralWindowShape::BlackmanHarris62;
    return spectral_window(coefficients,shape) && spectrum_.prepare(window,coefficients,0,true,factory);
}
void PreparedBandRhythmTransform::prepare_buffers() {
    derivative_.resize(bands_*config_.frame_size);current_.resize(bands_);previous_.resize(bands_);
    complex_.resize(2*bins());magnitude_.resize(bins());power_.resize(bins());
}
namespace {
bool valid_band_geometry(std::size_t bands,BandRhythmTransformConfig config) noexcept {
    const auto limit=std::size_t(std::numeric_limits<int>::max());
    if(!bands || config.frame_size<2 || config.frame_size>limit || !config.hop || config.hop>limit ||
       bands>std::numeric_limits<std::size_t>::max()/config.frame_size)return false;
    return true;
}
}

bool PreparedBandRhythmTransform::prepare(std::size_t bands,BandRhythmTransformConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(!valid_band_geometry(bands,config))return false;
    std::size_t bound=0;if(!storage_bytes(bands,config,bound,factory))return false;
    PreparedBandRhythmTransform next;next.bands_=bands;next.config_=config;
    std::vector<float> coefficients(config.frame_size);
    if(!next.prepare_spectrum(factory,coefficients))return false;
    next.prepare_buffers();
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
std::size_t PreparedBandRhythmTransform::frames(std::size_t input) const noexcept {
    return bands_ && input && input<=std::size_t(std::numeric_limits<int>::max())?1+(input-1)/config_.hop:0;
}
bool PreparedBandRhythmTransform::read_row(dsp_kernels::FloatMatrixSource rows,std::size_t at,std::span<float> output) noexcept {
    return rows.read(rows.context,at,output) &&
        std::all_of(output.begin(),output.end(),[](float x){return std::isfinite(x);});
}
bool PreparedBandRhythmTransform::differences(dsp_kernels::FloatMatrixSource rows,std::size_t count,std::size_t start) noexcept {
    std::fill(derivative_.begin(),derivative_.end(),0.f);
    if(start && !read_row(rows,start-1,previous_))return false;
    const auto valid=std::min(config_.frame_size,count-start);
    for(std::size_t time=0;time<valid;++time)if(!difference_row(rows,start+time,time))return false;
    return true;
}
bool PreparedBandRhythmTransform::difference_row(dsp_kernels::FloatMatrixSource rows,std::size_t at,std::size_t time) noexcept {
    if(!read_row(rows,at,current_))return false;
    if(at)for(std::size_t band=0;band<bands_;++band)
        derivative_[band*config_.frame_size+time]=current_[band]-previous_[band];
    current_.swap(previous_);return true;
}
bool PreparedBandRhythmTransform::transform_bands() noexcept {
    std::fill(power_.begin(),power_.end(),0.f);
    for(std::size_t band=0;band<bands_;++band){
        const auto values=std::span<const float>(derivative_).subspan(band*config_.frame_size,config_.frame_size);
        if(!spectrum_.process_frame(values,0,complex_) ||
           !dsp_kernels::cartesian_to_polar(complex_,magnitude_,{}))return false;
        for(std::size_t bin=0;bin<power_.size();++bin)power_[bin]+=magnitude_[bin]*magnitude_[bin];
    }
    return std::all_of(power_.begin(),power_.end(),[](float x){return std::isfinite(x);});
}
bool PreparedBandRhythmTransform::process(dsp_kernels::FloatMatrixSource rows,std::size_t count,
                                          dsp_kernels::FloatMatrixDestination output) noexcept {
    const auto n=frames(count);
    if(!n || !rows.read || !output.write)return false;
    for(std::size_t frame=0;frame<n;++frame){
        if(!differences(rows,count,frame*config_.hop) || !transform_bands() ||
           !output.write(output.context,frame,power_))return false;
    }
    return true;
}
}
