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
    struct Packet { uint64_t offset, timestamp, sample_position; uint32_t frames; bool discontinuity; };
    std::vector<float> _samples;
    std::array<Packet, 128> _packets{};
    std::atomic<uint64_t> _written{0}, _read{0}, _sample_read{0}, _dropped{0};
    uint64_t _sample_written = 0;
    bool _gap = false;
    uint32_t _rate = 0, _channels = 0;
    static void copy_samples(float *destination,const float *source,size_t count) noexcept {
        if(source)std::memcpy(destination,source,count*sizeof(float));
        else std::fill_n(destination,count,0.0f);
    }
    void unpack_samples(AudioFrame& frame,const Packet& packet,const float* source,size_t begin,size_t count) const {
        for(size_t i=0;i<count;++i){
            for(size_t channel=0;channel<_channels;++channel){
                frame.samples[channel*packet.frames+begin+i]=source[i*_channels+channel];
            }
        }
    }
  public:
    void reset(uint32_t rate, uint32_t channels) {
        // Admission bounds channels before opening a device. No callback allocation.
        _rate = rate; _channels = channels;
        _samples.assign(size_t(channels) * std::max(1u, rate / 4), 0);
        _written = 0; _read = 0; _sample_read = 0; _dropped = 0;
        _sample_written = 0; _gap = false;
    }
    bool push(const float *pcm, uint32_t frames, uint64_t timestamp, uint64_t sample_position) noexcept {
        const auto written = _written.load(std::memory_order_relaxed);
        const auto capacity = _channels ? _samples.size() / _channels : 0;
        if (!frames) return true;
        if (written - _read.load(std::memory_order_acquire) >= _packets.size() || frames > capacity ||
            _sample_written - _sample_read.load(std::memory_order_acquire) + frames > capacity) {
            _dropped.fetch_add(frames, std::memory_order_relaxed); _gap = true; return false;
        }
        const auto start = _sample_written % capacity;
        const auto first = std::min<uint64_t>(frames, capacity - start);
        // At most two contiguous copies, including packets crossing the ring end.
        copy_samples(_samples.data() + start * _channels, pcm, first * _channels);
        copy_samples(_samples.data(), pcm ? pcm + first * _channels : nullptr, (frames - first) * _channels);
        _packets[written % _packets.size()] = {_sample_written, timestamp, sample_position, frames, _gap};
        _gap = false; _sample_written += frames;
        _written.store(written + 1, std::memory_order_release); return true;
    }
    void mark_gap() noexcept { _gap = true; }
    bool pop(AudioFrame &frame) {
        const auto read = _read.load(std::memory_order_relaxed);
        if (read == _written.load(std::memory_order_acquire)) return false;
        const auto packet = _packets[read % _packets.size()];
        frame.sample_rate = _rate; frame.timestamp_ns = packet.timestamp; frame.discontinuity = packet.discontinuity;
        frame.channel_count = _channels; frame.frame_count = packet.frames; frame.sample_position = packet.sample_position;
        frame.samples.resize(size_t(packet.frames) * _channels);
        const auto start = packet.offset % (_samples.size() / _channels);
        const auto first = std::min<uint64_t>(packet.frames, _samples.size() / _channels - start);
        unpack_samples(frame,packet,_samples.data() + start * _channels,0,first);
        unpack_samples(frame,packet,_samples.data(),first,packet.frames-first);
        _sample_read.store(packet.offset + packet.frames, std::memory_order_release);
        _read.store(read + 1, std::memory_order_release); return true;
    }
    uint64_t dropped() const { return _dropped.load(std::memory_order_relaxed); }
};
} // namespace pipeline
