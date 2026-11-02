// SPDX-License-Identifier: ISC
// librosa feature/rhythm.py tempogram/fourier_tempogram/hybrid_tempogram,
// pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
namespace {
bool acf_storage(TempogramSequenceConfig config,std::span<const float> coefficients,std::size_t& bytes) noexcept {
    if(config.method==TempogramMethod::Fourier)return true;
    std::size_t nested=0;
    return PreparedAutocorrelationTempogram::storage_bytes(config.autocorrelation,coefficients,nested) &&
        prepared_storage::add_bytes(bytes,nested) && prepared_storage::add_array(bytes,config.autocorrelation.window.length,2*sizeof(float));
}
bool fourier_storage(TempogramSequenceConfig config,std::span<const float> coefficients,std::size_t& bytes) noexcept {
    if(config.method==TempogramMethod::Autocorrelation)return true;
    const auto window=config.autocorrelation.window;std::size_t nested=0;
    return PreparedFourierTempogram::storage_bytes(window,coefficients,window.length,false,nested) &&
        prepared_storage::add_bytes(bytes,nested) && prepared_storage::add_array(bytes,window.length/2+1,3*sizeof(float));
}
bool combination_storage(TempogramSequenceConfig config,std::size_t& bytes) noexcept {
    const auto length=config.autocorrelation.window.length,bins=length/2+1;
    if(config.method==TempogramMethod::Hybrid && config.autocorrelation.window.precision==NumericalPrecision::DesktopReference &&
       (config.interpolation.kind!=CurveInterpolation::Linear || length<3 || !prepared_storage::add_array(bytes,length-1,sizeof(double))))return false;
    if(config.method==TempogramMethod::Autocorrelation)return prepared_storage::add_array(bytes,length,2*sizeof(float));
    if(config.method==TempogramMethod::Fourier)return prepared_storage::add_array(bytes,bins,2*sizeof(float));
    std::size_t nested=0;
    return PreparedCurveInterpolation::storage_bytes(length,config.interpolation,nested) && prepared_storage::add_bytes(bytes,nested) &&
        prepared_storage::add_array(bytes,bins,sizeof(float));
}
}
bool PreparedTempogramSequence::storage_bytes(TempogramSequenceConfig config,std::span<const float> coefficients,std::size_t& bytes) noexcept {
    if(unsigned(config.method)>unsigned(TempogramMethod::Hybrid) || !std::isfinite(config.frame_rate) || config.frame_rate<=0)return false;
    config.autocorrelation.window.empty_centered_frame=true;std::size_t required=0;
    if(!acf_storage(config,coefficients,required) || !fourier_storage(config,coefficients,required) || !combination_storage(config,required))return false;
    bytes=required;return true;
}
bool PreparedTempogramSequence::prepare(TempogramSequenceConfig c,std::span<const float> coefficients) {
    ready_=false;
    std::size_t bound=0;if(!storage_bytes(c,coefficients,bound))return false;
    PreparedTempogramSequence next;
    if(!next.configure(c,coefficients))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedTempogramSequence::configure(TempogramSequenceConfig c,std::span<const float> coefficients) {
    if(unsigned(c.method)>unsigned(TempogramMethod::Hybrid) || !std::isfinite(c.frame_rate) || c.frame_rate<=0)return false;
    c.autocorrelation.window.empty_centered_frame=true;
    config_=c;
    if(!prepare_autocorrelation(coefficients) || !prepare_fourier(coefficients))return false;
    if(c.method!=TempogramMethod::Fourier)config_.autocorrelation=autocorrelation_.config();
    if(c.method==TempogramMethod::Hybrid && !interpolation_.prepare(acf_coordinates_,c.interpolation))return false;
    if(c.method==TempogramMethod::Hybrid && c.autocorrelation.window.precision==NumericalPrecision::DesktopReference && !prepare_reference_hybrid())return false;
    row_.resize(c.method==TempogramMethod::Fourier?fourier_row_.size():coordinates_.size());return true;
}
bool PreparedTempogramSequence::prepare_autocorrelation(std::span<const float> coefficients) {
    if(config_.method==TempogramMethod::Fourier)return true;
    if(!autocorrelation_.prepare(config_.autocorrelation,coefficients))return false;
    const auto count=config_.autocorrelation.window.length;
    acf_row_.resize(count);acf_coordinates_.resize(count);
    return autocorrelation_tempo_frequencies(config_.frame_rate,acf_coordinates_);
}
bool PreparedTempogramSequence::prepare_fourier(std::span<const float> coefficients) {
    if(config_.method==TempogramMethod::Autocorrelation){coordinates_=acf_coordinates_;return true;}
    const auto& window=config_.autocorrelation.window;
    if(!fourier_.prepare(window,coefficients))return false;
    fourier_row_.resize(2*fourier_.bins());coordinates_.resize(fourier_.bins());
    return fourier_tempo_frequencies(config_.frame_rate,window.length,coordinates_);
}
std::size_t PreparedTempogramSequence::frames(std::size_t count) const noexcept {
    if(row_.empty())return 0;
    if(config_.method==TempogramMethod::Autocorrelation)return autocorrelation_.frames(count);
    if(config_.method==TempogramMethod::Fourier)return fourier_.frames(count);
    return std::min(autocorrelation_.frames(count),fourier_.frames(count));
}
bool PreparedTempogramSequence::frame_count(TempogramSequenceConfig c,std::size_t count,std::size_t& out) noexcept {
    if(unsigned(c.method)>unsigned(TempogramMethod::Hybrid))return false;
    c.autocorrelation.window.empty_centered_frame=true;
    std::size_t frames=0;if(!PreparedTemporalWindow::frame_count(c.autocorrelation.window,count,frames))return false;
    out=c.method!=TempogramMethod::Fourier && c.autocorrelation.window.center?std::min(count,frames):frames;return true;
}
bool PreparedTempogramSequence::evaluate(std::span<const float> envelope,std::size_t frame) noexcept {
    ready_=false;
    if(frame>=frames(envelope.size()))return false;
    if(config_.method!=TempogramMethod::Fourier &&
        !autocorrelation_.process_frame(envelope,frame,acf_row_))return false;
    if(config_.method!=TempogramMethod::Autocorrelation &&
        !fourier_.process_frame(envelope,frame,fourier_row_))return false;
    if(!combine())return false;
    active_frame_=frame;ready_=true;return true;
}
bool PreparedTempogramSequence::combine() noexcept {
    if(config_.method==TempogramMethod::Autocorrelation){std::copy(acf_row_.begin(),acf_row_.end(),row_.begin());return true;}
    if(config_.method==TempogramMethod::Fourier){std::copy(fourier_row_.begin(),fourier_row_.end(),row_.begin());return true;}
    if(config_.autocorrelation.window.precision==NumericalPrecision::DesktopReference)return combine_reference_hybrid();
    return interpolation_.set_values(acf_row_) && hybrid_tempogram_frame(coordinates_,fourier_row_,interpolation_,row_);
}
bool PreparedTempogramSequence::process_frame(std::span<const float> envelope,std::size_t frame,std::span<float> output) noexcept {
    ready_=false;
    if(output.size()!=row_.size() || !evaluate(envelope,frame))return false;
    if(output.data()!=row_.data())std::copy(row_.begin(),row_.end(),output.begin());
    return true;
}
bool PreparedTempogramSequence::process(std::span<const float> envelope,FloatMatrixDestination output) noexcept {
    ready_=false;
    if(row_.empty() || !output.write)return false;
    const auto count=frames(envelope.size());
    if(!count)return false;
    for(std::size_t frame=0;frame<count;++frame)
        if(!evaluate(envelope,frame) || !output.write(output.context,frame,row_)){ready_=false;return false;}
    return true;
}
bool PreparedTempogramSequence::process_channels(std::span<const std::span<const float>> channels,FloatMatrixBatchDestination output) noexcept {
    ready_=false;
    if(row_.empty() || !output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        if(!frames(channels[channel].size())){ready_=false;return false;}
        for(std::size_t frame=0;frame<frames(channels[channel].size());++frame)
            if(!evaluate(channels[channel],frame) || !output.write(output.context,channel,frame,row_)){ready_=false;return false;}
    }
    return true;
}
TempogramFrameView PreparedTempogramSequence::view() const noexcept {
    if(!ready_)return {};
    const auto fourier_bpm=config_.method==TempogramMethod::Autocorrelation?std::span<const float>{}:std::span<const float>(coordinates_);
    const auto origin=double(active_frame_)*double(config_.autocorrelation.window.hop)/double(config_.frame_rate);
    return {row_,coordinates_,acf_coordinates_,fourier_bpm,autocorrelation_.view(),fourier_.view(),active_frame_,origin,complex_output()};
}
std::size_t PreparedTempogramSequence::workspace_bytes() const noexcept {
    return (fourier_row_.capacity()+acf_row_.capacity()+coordinates_.capacity()+acf_coordinates_.capacity()+row_.capacity())*sizeof(float)+
        reference_coordinates_.capacity()*sizeof(double)+
        fourier_.workspace_bytes()+autocorrelation_.workspace_bytes()+interpolation_.workspace_bytes();
}
}
