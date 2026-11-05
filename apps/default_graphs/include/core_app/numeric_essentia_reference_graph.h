// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <core_app/numeric_default_graph.h>
#include <cstddef>
#include <cstdint>
enum class EssentiaReferenceMethod {Degara,MultiFeature};
// Complete independent eight-second source sessions; output ticks are seconds
// relative to each session. Native PCM conversion is an explicit graph stage.
NumericExampleGraph numeric_essentia_reference_example(
    EssentiaReferenceMethod method=EssentiaReferenceMethod::MultiFeature);
// One live session with Degara ticks at 50:1 and MultiFeature ticks at 60:0.
// The complex candidate is shared. Summaries are at 62 and 61, respectively.
NumericExampleGraph numeric_essentia_combined_reference_example();

struct EssentiaFiniteOptions {
    std::size_t input_samples;
    std::size_t maximum_native_pcm_samples;
    std::uint32_t input_sample_rate=44100;
};
// Two explicit ingresses in one prepared graph. Feed all input_samples to1,
// copy output2 into cold owning storage, then inject its actual PCM once at3.
// Native44100 only; no synthetic EOF samples or executor callback reentry.
NumericExampleGraph numeric_essentia_finite_reference_example(EssentiaFiniteOptions,
    EssentiaReferenceMethod method=EssentiaReferenceMethod::MultiFeature);
