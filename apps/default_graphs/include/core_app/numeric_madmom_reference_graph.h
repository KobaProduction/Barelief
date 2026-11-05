#pragma once
#include <core_app/numeric_default_graph.h>
#include <cstddef>

// Classical spectral evidence; no RNN probabilities or pretrained models.
enum class MadmomReferenceMode { Fixed, Local, Crf };
// Native input Fs is explicit; analysis is resampled to 48000 Hz / hop 240.
// Beat seconds refer to the current 2000-frame ODF snapshot, not absolute time.
NumericExampleGraph numeric_madmom_reference_example(
    std::uint32_t source_rate=48000, MadmomReferenceMode mode=MadmomReferenceMode::Fixed);

// Finite, zero-origin native 48000-Hz research session. Source 1 supplies PCM;
// collect scalar ODF output 3 and submit its entire fixed-F vector to source 31
// exactly once after PCM completion, outside callbacks, on the same plan.
// F=1+(pcm_samples-2048)/240; no rolling history/stride or implicit EOF flush.
// Input 31 has an explicit 200-Hz ODF clock and regular k/200-second values;
// its point stamp carries no PCM sample interval. Other native Fs are rejected.
// Whole CRF preserves Double Hann/product/FFT, publishing Float32 Re/Im before
// magnitude. Fixed/Local and rolling recipes retain their Float32 frontend.
NumericExampleGraph numeric_madmom_whole_reference_example(
    std::size_t pcm_samples, MadmomReferenceMode mode=MadmomReferenceMode::Crf,
    std::uint32_t source_rate=48000);
