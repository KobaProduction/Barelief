// SPDX-License-Identifier: BSD-2-Clause
// madmom features/onsets.py peak_picking, audio/signal.py smooth,
// pinned27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// Full notice: packaging/licenses/madmom.txt.
#include <dsp_kernels/activation_peak_matrix.h>
#include <algorithm>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedActivationPeakMatrix::prepare(std::size_t frames,std::size_t channels,
    ActivationPeakConfig config,std::span<const float> kernel) {
    std::size_t bound=0;if(!storage_bytes(frames,channels,config,kernel,bound))return false;
    PreparedActivationPeakMatrix next;
    config.smoothing_crop=SameConvolutionCrop::Input;
    if(!next.peaks_.prepare(frames,config,kernel))return false;
    next.row_.resize(channels);next.column_.resize(frames);next.mask_.resize(frames);
    next.positions_.resize(frames*channels);next.maximum_frames_=frames;next.maximum_channels_=channels;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedActivationPeakMatrix::read_column(FloatMatrixSource source,std::size_t frames,
    std::size_t channels,std::size_t channel) noexcept {
    auto row=std::span(row_).first(channels);
    for(std::size_t frame=0;frame<frames;++frame){
        if(!source.read(source.context,frame,row))return false;
        column_[frame]=row[channel];
    }
    return true;
}
bool PreparedActivationPeakMatrix::process_channel(FloatMatrixSource source,std::size_t frames,
    std::size_t channels,std::size_t channel,std::size_t& count) noexcept {
    if(!read_column(source,frames,channels,channel) ||
       !peaks_.process(std::span(column_).first(frames),std::span(mask_).first(frames)))return false;
    for(std::size_t frame=0;frame<frames;++frame)
        if(mask_[frame])positions_[count++]={frame,channel};
    return true;
}
bool PreparedActivationPeakMatrix::process(FloatMatrixSource source,std::size_t frames,std::size_t channels) noexcept {
    count_=0;
    if(!maximum_frames_ || frames>maximum_frames_ || channels>maximum_channels_ || !source.read)return false;
    if(!frames || !channels)return true;
    std::size_t count=0;
    for(std::size_t channel=0;channel<channels;++channel)
        if(!process_channel(source,frames,channels,channel,count))return false;
    std::sort(positions_.begin(),positions_.begin()+std::ptrdiff_t(count),[](auto a,auto b){
        return a.frame<b.frame || (a.frame==b.frame && a.channel<b.channel);
    });
    count_=count;return true;
}
}
