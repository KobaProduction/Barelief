#pragma once
#include <core_app/numeric_default_graph.h>
#include <cstdint>

// Classical spectral evidence; no pretrained model dependency.
// One rolling SuperFlux snapshot drives the Fixed, Local and CRF decoders.
// Analysis is resampled to 48000 Hz / hop 240. Beat seconds are snapshot-relative.
NumericExampleGraph numeric_madmom_combined_reference_example(std::uint32_t source_rate=48000);
