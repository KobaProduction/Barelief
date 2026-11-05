// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/tempogram.h>

namespace rhythm_reference_ports {
// Synchronous output delivery, not storage or an executor. A consumer must copy
// into its own bounded output if it needs the values after the callback. It must
// not reenter/mutate the producer or alias its working buffers/borrowed PCM.
// The port/context are borrowed for one process() call and are never retained.
// A callback publishes a completed frame, not acceptance of the entire batch:
// a later frame/finalizer can fail. All values are producer-owned, read-only
// borrowed spans, invalidated by the next frame/process/reset (including failed
// processing), successful prepare, move or destruction. Last-frame getters do
// not retain callback arguments, allocate memory or recompute any operation.
template<class Frame> struct FrameStageOutputPort {
    const void* context=nullptr;
    void (*write)(const void*,const Frame&) noexcept=nullptr;
    void publish(const Frame& frame) const noexcept {if(write)write(context,frame);}
};
struct SpectralFrameStages {
    std::size_t frame_index=0;
    double first_sample=0; // Actual frame start in the caller PCM coordinates.
    // Time is first_sample/Fs; frame_index is not a timestamp. The source ODF
    // can have algorithmic latency even though its source frame is identified.
    float sample_rate=0;
    dsp_kernels::TemporalWindowConfig framing;
    bool zero_phase=false;
    std::span<const float> window_coefficients,magnitude,phase;
    // These source recipes explicitly select sqrt(Re*Re+Im*Im), not hypot,
    // and unconditional atan2(Im,Re), including signed-zero endpoints. They do
    // not ask the polar primitive to replace real FFT endpoint phase values.
    // Raw [bin][Re,Im], DC/Nyquist included; physical bin Hz=bin*Fs/fft_size.
    // windowed_input and transform_input show normalization/zero-phase embedding.
    dsp_kernels::FourierTempogramView fourier;
    bool valid() const noexcept {return fourier.fft_size!=0;}
};
// Geometry comes from the recipe's explicit framing settings, not graph metadata.
// Legacy conditioned frames use the outer PCM framing with the inner FFT view.
inline SpectralFrameStages spectral_frame_stages(const dsp_kernels::PreparedFourierTempogram& spectrum,
    dsp_kernels::TemporalWindowConfig framing,std::size_t index,float sample_rate,bool zero_phase,
    std::span<const float> magnitude,std::span<const float> phase={}) noexcept {
    const auto offset=framing.center?framing.length/2+(framing.odd_center_ceil && framing.length%2):0;
    return {index,double(index)*double(framing.hop)-double(offset),sample_rate,framing,zero_phase,
        spectrum.window_coefficients(),magnitude,phase,spectrum.view()};
}
}
