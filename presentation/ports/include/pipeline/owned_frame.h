#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <signal_contracts/frame.h>
namespace pipeline {
// Transport-independent owned payload. Timestamps retain the source clock domain.
using FrameType = signal_contracts::FrameType;
struct TypedFrame {
    FrameType type = FrameType::PCM;
    uint64_t sequence = 0, generation = 0, epoch = 0, graph_revision = 0;
    uint64_t source_sequence = 0;
    uint64_t producer_epoch = 0;
    int64_t timestamp_ns = 0;
    size_t sample_rate = 0;
    float frequency_step = 0, peak_hz = 0, magnitude = 0;
    size_t fft_size = 0, bands = 0, history_capacity = 0;
    std::string stream_id;
    bool discontinuity = false, bark_weighted = false;
    bool available = true;
    std::vector<float> left, right, values;
    std::vector<std::vector<float>> history;
};
} // namespace pipeline
