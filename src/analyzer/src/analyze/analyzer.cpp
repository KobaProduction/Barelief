#include "core/performance.h"
#include "analyzer/analyze/analyzer.h"

#include <algorithm>
#include <cmath>
#include <chrono>

#include "dsps_wind_hann.h"

Samples::Samples(size_t size) : left(size), right(size) {
}

Amplitudes::Amplitudes(size_t size) {
    left.resize(size, 0);
    right.resize(size, 0);
}

void fft_to_mel(const std::vector<float> &spectrum, size_t rate, std::vector<float> &output, size_t bands) {
    // Compatibility helper. Hot graph evaluation owns and reuses its filter bank.
    dsp_core::MelFilterBank filter;
    filter.prepare(spectrum.size(), rate, bands);
    output.resize(bands);
    filter.process(spectrum, output);
}

uint64_t Analyzer::_task() {
    std::vector<float> left(_samples_size), right(_samples_size), weights, hann;
    SpectrumFrame frame;
    dsp_core::StereoProcessor processor;
    size_t prepared_rate = SIZE_MAX;
    bool mel_enabled = true;
    bool window_only = false, pcm_only = false;
    uint64_t previous_frame_time=0;
    while (_running.load()) {

        left.clear(); right.clear();
        size_t rate = 0;
        update_handler_t handler;
        std::function<void(const SpectrumFrame &)> frame_handler;
        on_windowed_signal_handler_t windowed_handler;
        {
            std::unique_lock lock(_mutex);
            _window_signal_ready.wait(lock, [this] { return _pending_size || !_running.load(); });
            if (!_running.load()) break;
            core::perf::Scope trace(core::perf::Stage::WindowSnapshot);
            if (_pending_size) {
                auto &pending = _pending[_pending_head];
                left.swap(pending.left); right.swap(pending.right);
                _pending_head = (_pending_head + 1) % pending_capacity; --_pending_size;
                frame.input_timestamp_ns = pending.input_timestamp_ns;
                frame.pipeline_epoch = pending.epoch;
                frame.ingress_serial = pending.ingress_serial;
                frame.discontinuity = _force_discontinuity || pending.sequence != _sequence + 1;
                _force_discontinuity = false;
                frame.sequence = pending.sequence; _sequence = pending.sequence;
                frame.dropped_windows = _dropped_windows;
                weights = _bark_scale;
                hann = window;
                rate = _sample_rate;
                mel_enabled = _mel_enabled;
                window_only = _window_only;pcm_only = _pcm_only;

                handler = _handler;
                frame_handler = _frame_handler;
                windowed_handler = _windowed_signal_handler;
            }
        }
        if (!left.empty()) {
            core::perf::Scope work(core::perf::Stage::AnalysisWork);
            const auto frame_time=core::perf::now();
            if(previous_frame_time) core::perf::record(core::perf::Stage::AnalysisGap,previous_frame_time,frame_time-previous_frame_time);
            previous_frame_time=frame_time;
            frame.audio_samples=left.size(); frame.audio_peak=0;
            for (size_t i=0;i<left.size();++i) frame.audio_peak=std::max({frame.audio_peak,std::abs(left[i]),std::abs(right[i])});
            frame.sample_rate = rate;
            frame.frequency_step = float(rate) / float(_samples_size);
            frame.window_signal = left;
            frame.window = hann;

            frame.timestamp_ns = frame.input_timestamp_ns;
            if (window_only) {
                frame.raw_window = true;
                frame.raw_pcm = pcm_only;
                frame.left = left; frame.right = right;
                frame.windowed_signal.clear();
                if (frame_handler) frame_handler(frame);
                if (handler) handler();
                continue;
            }
            if (prepared_rate != rate) {
                if (!processor.prepare(_samples_size, rate)) return 1;
                prepared_rate = rate;
            }
            frame.left.resize(_amplitudes_size); frame.right.resize(_amplitudes_size);
            frame.mel_left.resize(mel_enabled ? 50 : 0); frame.mel_right.resize(mel_enabled ? 50 : 0);
            { core::perf::Scope trace(core::perf::Stage::FFT);
              if (!processor.process(left, right, hann, weights, frame.left, frame.right)) return 1; }
            if (mel_enabled) {
                core::perf::Scope trace(core::perf::Stage::StereoMel);
                if (!processor.process_mel(frame.left, frame.right, frame.mel_left, frame.mel_right)) return 1;
            }
            frame.windowed_signal = left;
            {
                std::lock_guard lock(_mutex);
                amplitudes.left = frame.left; amplitudes.right = frame.right;
                amplitudes_test.left = frame.mel_left; amplitudes_test.right = frame.mel_right;
            }
            // Consumer callbacks and FFT never hold the capture mutex.
            if (windowed_handler) windowed_handler(left);
            if (frame_handler) frame_handler(frame);
            if (handler) handler();
        }
    }
    return 0;
}

std::vector<float> Analyzer::merge_spectra(const std::vector<float> &left, const std::vector<float> &right, SpectrumMerge mode) {
    std::vector<float> result(std::min(left.size(), right.size()));
    for (size_t i = 0; i < result.size(); ++i) {
        if (mode == SpectrumMerge::Maximum) result[i] = std::max(left[i], right[i]);
        else if (mode == SpectrumMerge::Energy) result[i] = std::hypot(left[i], right[i]) / std::sqrt(2.0f);
        else result[i] = (left[i] + right[i]) * 0.5f;
    }
    return result;
}
void Analyzer::set_mel_enabled(bool enabled) {
    std::lock_guard lock(_mutex); _mel_enabled = enabled;
}
void Analyzer::set_window_only(bool enabled) {
    std::lock_guard lock(_mutex); _window_only = enabled;
}
void Analyzer::set_pcm_only(bool enabled) {
    std::lock_guard lock(_mutex); _pcm_only = enabled; _window_only = enabled;
}
void Analyzer::set_stream_epoch(uint64_t epoch, bool preserve_continuity) {
    std::lock_guard lock(_mutex); _stream_epoch = epoch;
    if(preserve_continuity) {
        for(size_t i=0;i<_pending_size;++i) _pending[(_pending_head+i)%pending_capacity].epoch=epoch;
        return;
    }
    _pending_head = _pending_size = 0; _force_discontinuity = true;
}
void Analyzer::set_ingress_serial(uint64_t serial) {
    std::lock_guard lock(_mutex); _ingress_serial=serial;
    _pending_head=_pending_size=0;_force_discontinuity=true;
}
void Analyzer::stop() {
    {
        std::lock_guard lock(_mutex);
        _running = false;
    }
    _window_signal_ready.notify_all();
    AbstractTask::stop();
}
void Analyzer::set_frame_handler(std::function<void(const SpectrumFrame &)> handler) {
    std::lock_guard lock(_mutex); _frame_handler = std::move(handler);
}

Analyzer::Analyzer() {
    _samples_size = 4096;
    _sample_rate = 0;
    _amplitudes_size = _samples_size / 2;

    generate_window();

    samples = Samples(_samples_size);
    for (auto &pending : _pending) { pending.left.resize(_samples_size); pending.right.resize(_samples_size); }
    amplitudes = Amplitudes(_amplitudes_size);
    amplitudes_test = Amplitudes(50);
    _bark_scale.resize(_amplitudes_size, 1.0f);

}

std::vector<float> Analyzer::average_channels(const std::vector<float> &left, const std::vector<float> &right) {
    std::vector<float> result(std::min(left.size(), right.size()));
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = (left[i] + right[i]) / 2.0f;
    }
    return result;
}

void Analyzer::add_samples(const std::vector<float> &left, const std::vector<float> &right, int64_t timestamp_ns) {
    std::lock_guard lock(_mutex);
    _input_timestamp_ns = timestamp_ns ? timestamp_ns : std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto count = std::min(left.size(), right.size());
    if (_pcm_only) {
        if (_pending_size == pending_capacity) {
            _pending_head = (_pending_head + 1) % pending_capacity; --_pending_size; ++_dropped_windows;
        }
        auto &pending = _pending[(_pending_head + _pending_size) % pending_capacity];
        pending.left=left;pending.right=right;pending.input_timestamp_ns=_input_timestamp_ns;
        pending.sequence=++_queued_sequence;pending.epoch=_stream_epoch;pending.ingress_serial=_ingress_serial;++_pending_size;
        _window_signal_ready.notify_one();return;
    }
    const auto rate = _sample_rate ? _sample_rate : 48000;
    size_t offset = 0;
    while (offset < count) {
        if (!_hop_size) {
            _hop_size = rate / 60;
            _hop_remainder += rate % 60;
            if (_hop_remainder >= 60) { ++_hop_size; _hop_remainder -= 60; }
            _hop_size = std::max(size_t{1}, _hop_size);
        }
        const auto chunk = std::min(count - offset, _hop_size - _hop_progress);
        samples.left.append_data(left.data() + offset, chunk);
        samples.right.append_data(right.data() + offset, chunk);
        offset += chunk; _hop_progress += chunk;
        if (_hop_progress == _hop_size) {
            if (_pending_size == pending_capacity) {
                _pending_head = (_pending_head + 1) % pending_capacity; --_pending_size; ++_dropped_windows;
            }
            auto &pending = _pending[(_pending_head + _pending_size) % pending_capacity];
            pending.left = samples.left.get_window(); pending.right = samples.right.get_window();
            pending.input_timestamp_ns = _input_timestamp_ns - static_cast<int64_t>(((count - offset) * 1000000000ULL) / rate);
            pending.sequence = ++_queued_sequence; ++_pending_size;
            pending.epoch = _stream_epoch;
            pending.ingress_serial = _ingress_serial;
            _hop_progress = 0; _hop_size = 0;
        }
    }
    if (_pending_size) _window_signal_ready.notify_one();
}

void Analyzer::set_update_handler(update_handler_t handler) {
    std::lock_guard lock(_mutex);
    _handler = std::move(handler);
}

void Analyzer::set_update_window_function_handler(on_update_window_function_handler_t handler) {
    std::vector<float> current;
    { std::lock_guard lock(_mutex); _update_window_function_handler = handler; current = window; }
    if (handler) handler(current);
}

void Analyzer::set_windowed_signal_handler(on_windowed_signal_handler_t handler) {
    std::lock_guard lock(_mutex);
    _windowed_signal_handler = std::move(handler);
}

void Analyzer::generate_bark_scale(float *scale_ptr, size_t scale_size, float frequency_step) {
    float base;
    for (int i = 0; i < scale_size; i++) {
        base = static_cast<float>(i) * (frequency_step / 650);
        scale_ptr[i] = 7 * logf(base + sqrtf(1 + base * base));
    }
}

void Analyzer::update_sample_rate(size_t sample_rate) {
    std::lock_guard lock(_mutex);
    if (sample_rate == _sample_rate) return;
    if (_sample_rate) _force_discontinuity = true;
    _sample_rate = sample_rate;
    samples.left.clear(); samples.right.clear();
    _pending_head = _pending_size = 0; _hop_progress = _hop_remainder = _hop_size = 0;
    generate_bark_scale(_bark_scale.data(), _bark_scale.size(), get_freq_step());

}

float Analyzer::get_freq_step() {
    std::lock_guard lock(_mutex);
    return float(_sample_rate) / float(_samples_size);
}

void Analyzer::generate_volume_scale(float *scale_ptr, size_t scale_size, float frequency_step) {
    for (size_t i = 0; i < scale_size; ++i) {
        float f = i * frequency_step;

        if (f <= 0.0f) {
            scale_ptr[i] = 0.0f;
            continue;
        }

        float f2 = f * f;

        float num = powf(12200.0f, 2.0f) * f2 * f2;

        float den =
                (f2 + powf(20.6f, 2.0f)) *
                sqrtf((f2 + powf(107.7f, 2.0f)) *
                      (f2 + powf(737.9f, 2.0f))) *
                (f2 + powf(12200.0f, 2.0f));

        float A = 20.0f * log10f(num / den) + 2.0f;

        // перевод dB -> линейный коэффициент
        scale_ptr[i] = powf(10.0f, A / 20.0f);
    }
}

void Analyzer::generate_window() {
    std::lock_guard lock(_mutex);
    window.resize(_samples_size);
    dsps_wind_hann_f32(window.data(), window.size());
    if (_update_window_function_handler) {
        _update_window_function_handler(window);
    }
}

void Analyzer::use_window(std::vector<float> &samples_) {
    std::lock_guard lock(_mutex);
    if (samples_.empty()) return;
    if (window.size() not_eq samples_.size()) generate_window();
    for (size_t i = 0; i < samples_.size(); ++i) {
        samples_[i] = window[i] * samples_[i];
    }
    if (_windowed_signal_handler) {
        _windowed_signal_handler(samples_);
    }
}

std::vector<float> Analyzer::get_samples_snapshot() const {
    std::lock_guard lock(_mutex);
    return samples.left.get_window();
}

std::vector<float> Analyzer::get_window_snapshot() const {
    std::lock_guard lock(_mutex);
    return window;
}

void Analyzer::reset_samples() {
    std::lock_guard lock(_mutex); samples.left.clear(); samples.right.clear();
    _pending_head = _pending_size = 0; _hop_progress = _hop_remainder = _hop_size = 0;
    _force_discontinuity = true;
}
