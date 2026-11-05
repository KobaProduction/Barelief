#pragma once
#include <QString>
#include <cstddef>
#include <cstdint>

namespace rhythm_replay {
struct Options {
    QString input, output_directory;
    std::size_t packet_samples=256;
    unsigned analysis_rate=60;
    bool reference_trace=false;
};
struct Result {
    bool success=false;
    QString error;
    std::uint64_t input_samples=0, analysis_frames=0;
    unsigned source_rate=0;
};
// Bounded file ingress drives the shipped graph, including its declared Speex
// resampler and measured amplitude correction. No playback or capture device.
// Output directory must be new or empty. Labels/known tempo are never inputs.
Result run(const Options&);
}
