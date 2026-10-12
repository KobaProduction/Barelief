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
#include "pipeline/numeric_delivery.h"
#include "core/runtime_state_model.h"
enum class SpectrumMerge { Mean, Maximum, Energy };
namespace pipeline {
struct AudioFrame {
    // Owning Float32 capture-adapter input, before the declared numeric ingress.
    uint64_t sequence = 0;
    int64_t timestamp_ns = 0;
    size_t sample_rate = 0;
    uint64_t generation = 0;
    bool discontinuity = false;
    // Channel-major capture storage: samples[channel * frame_count + sample].
    // Lanes have no semantic speaker roles. Numeric ingress emits each separately.
    uint32_t channel_count = 0;
    size_t frame_count = 0;
    uint64_t sample_position = 0;
    std::vector<float> samples;
};

struct NodePreview {
    // Small GUI diagnostic projection, never a processing frame or port contract.
    size_t values = 0;
    float peak = 0;
};
using State = core::RuntimeState;
struct ComponentStatus {
    State state = State::Stopped;
    QString error;
    uint64_t revision = 0;
};
} // namespace pipeline
