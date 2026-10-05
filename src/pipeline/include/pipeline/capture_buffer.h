#pragma once
#include "pipeline/audio_source.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

namespace pipeline {
// One device callback producer, one consumer; reset only while both are stopped.
class CaptureBuffer {
    struct Packet { uint64_t offset, timestamp; uint32_t frames; bool discontinuity; };
    std::vector<float> _samples;
    std::array<Packet, 128> _packets{};
    std::atomic<uint64_t> _written{0}, _read{0}, _sample_read{0}, _dropped{0};
    uint64_t _sample_written = 0;
    bool _gap = false;
    uint32_t _rate = 0;
  public:
    void reset(uint32_t rate) {
        _rate = rate; _samples.assign(2 * std::max(1u, rate / 4), 0);
        _written = 0; _read = 0; _sample_read = 0; _dropped = 0;
        _sample_written = 0; _gap = false;
    }
    bool push(const float *pcm, uint32_t frames, uint64_t timestamp) noexcept {
        const auto written = _written.load(std::memory_order_relaxed);
        const auto capacity = _samples.size() / 2;
        if (!frames) return true;
        if (written - _read.load(std::memory_order_acquire) >= _packets.size() || frames > capacity ||
            _sample_written - _sample_read.load(std::memory_order_acquire) + frames > capacity) {
            _dropped.fetch_add(frames, std::memory_order_relaxed); _gap = true; return false;
        }
        const auto start = _sample_written % capacity;
        const auto first = std::min<uint64_t>(frames, capacity - start);
        const auto copy = [](float *destination, const float *source, size_t count) noexcept {
            if (source) std::memcpy(destination, source, count * sizeof(float));
            else std::fill_n(destination, count, 0.0f);
        };
        // At most two contiguous copies, including packets crossing the ring end.
        copy(_samples.data() + start * 2, pcm, first * 2);
        copy(_samples.data(), pcm ? pcm + first * 2 : nullptr, (frames - first) * 2);
        _packets[written % _packets.size()] = {_sample_written, timestamp, frames, _gap};
        _gap = false; _sample_written += frames;
        _written.store(written + 1, std::memory_order_release); return true;
    }
    void mark_gap() noexcept { _gap = true; }
    bool pop(AudioFrame &frame) {
        const auto read = _read.load(std::memory_order_relaxed);
        if (read == _written.load(std::memory_order_acquire)) return false;
        const auto packet = _packets[read % _packets.size()];
        frame.sample_rate = _rate; frame.timestamp_ns = packet.timestamp; frame.discontinuity = packet.discontinuity;
        frame.left.resize(packet.frames); frame.right.resize(packet.frames);
        const auto start = packet.offset % (_samples.size() / 2);
        const auto first = std::min<uint64_t>(packet.frames, _samples.size() / 2 - start);
        const auto unpack = [&](const float *source, size_t begin, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                frame.left[begin + i] = source[i * 2];
                frame.right[begin + i] = source[i * 2 + 1];
            }
        };
        unpack(_samples.data() + start * 2, 0, first);
        unpack(_samples.data(), first, packet.frames - first);
        _sample_read.store(packet.offset + packet.frames, std::memory_order_release);
        _read.store(read + 1, std::memory_order_release); return true;
    }
    uint64_t dropped() const { return _dropped.load(std::memory_order_relaxed); }
};
} // namespace pipeline
