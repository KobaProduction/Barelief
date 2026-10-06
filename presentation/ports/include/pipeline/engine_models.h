#pragma once
// Owned desktop delivery DTOs. No capture, executor or transport implementation.
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#include <QMap>
#include <QString>
#include <QMetaType>
#include "pipeline/owned_frame.h"
#include "core/runtime_state_model.h"
enum class SpectrumMerge { Mean, Maximum, Energy };
struct SpectrumFrame {
    uint64_t sequence = 0;
    int64_t timestamp_ns = 0;
    int64_t input_timestamp_ns = 0;
    size_t audio_samples = 0;
    float audio_peak = 0;
    size_t sample_rate = 0;
    float frequency_step = 0;
    uint64_t generation = 0;
    uint64_t dropped_windows = 0;
    bool discontinuity = false;
    bool raw_window = false;
    bool raw_pcm = false;
    uint64_t pipeline_epoch = 0;
    uint64_t ingress_serial = 0;
    std::vector<float> window_signal, window, windowed_signal;
    std::vector<float> left, right, mel_left, mel_right;
};

namespace pipeline {
struct AudioFrame {
    uint64_t sequence = 0;
    int64_t timestamp_ns = 0;
    size_t sample_rate = 0;
    uint64_t generation = 0;
    bool discontinuity = false;
    std::vector<float> left, right;
};

struct NodePreview {
    size_t values = 0;
    float peak = 0;
};
using State = core::RuntimeState;
struct ComponentStatus {
    State state = State::Stopped;
    QString error;
    uint64_t revision = 0;
};
struct HistorySnapshot {
    uint64_t graph_revision = 0;
    std::vector<std::vector<float>> values;
    uint64_t analyzer_generation = 0;
};
struct ViewFrame {
    SpectrumFrame spectrum;
    std::vector<float> fft, mel;
    QMap<int, NodePreview> previews;
    bool spectrogram = true;
    uint64_t graph_revision = 0;
    uint64_t ready_timestamp_ns = 0;
    uint64_t epoch = 0;
    std::vector<float> window_signal, window, windowed_signal;
    float frequency_step = 0;
    std::shared_ptr<const HistorySnapshot> history;
    QMap<int, TypedFrame> views;
};
} // namespace pipeline
