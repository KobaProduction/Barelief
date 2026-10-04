#ifndef QT_VISUALIZATION_ANALYZER_H
#define QT_VISUALIZATION_ANALYZER_H

#include <vector>
#include <mutex>
#include <functional>
#include <array>

#include "core/abstract/abs_task.h"
#include "fft.h"
#include "dsp_core/dsp.h"
#include "audio_window.h"

class Samples {
public:
    AudioWindow left;
    AudioWindow right;

    explicit Samples(size_t size = 0);
};

class Amplitudes {
public:
    std::vector<float> left;
    std::vector<float> right;

    explicit Amplitudes(size_t size = 0);
};

void fft_to_mel(const std::vector<float> &spectrum, size_t sample_rate, std::vector<float> &output, size_t bands = 50);

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
    std::vector<float> samples, window, post_window;
    std::vector<float> left, right, mel_left, mel_right;
};

class Analyzer final : public AbstractTask {
public:
    typedef std::function<void(void)> update_handler_t;
    typedef std::function<void(const std::vector<float> &window)> on_update_window_function_handler_t;
    typedef std::function<void(const std::vector<float> &window)> on_samples_after_window_function_handler_t;
protected:
    mutable std::recursive_mutex _mutex;
    std::condition_variable_any _samples_ready;

    update_handler_t _handler = nullptr;
    std::function<void(const SpectrumFrame &)> _frame_handler;
    uint64_t _sequence = 0;
    int64_t _input_timestamp_ns = 0;
    on_update_window_function_handler_t _update_window_function_handler = nullptr;
    on_samples_after_window_function_handler_t _samples_after_window_function_handler = nullptr;

    bool _mel_enabled = true;
    struct PendingWindow {
        std::vector<float> left, right;
        int64_t input_timestamp_ns = 0;
        uint64_t sequence = 0;
    };
    static constexpr size_t pending_capacity = 16;
    std::array<PendingWindow, pending_capacity> _pending;
    size_t _pending_head = 0, _pending_size = 0;
    size_t _hop_progress = 0;
    size_t _hop_remainder = 0;
    size_t _hop_size = 0;
    uint64_t _queued_sequence = 0;
    uint64_t _dropped_windows = 0;
    bool _force_discontinuity = false;
    size_t _sample_rate{};
    size_t _samples_size{};
    size_t _amplitudes_size{};
    std::vector<float> _bark_scale;

    uint64_t _task() override;

public:
    Samples samples;
    std::vector<float> window;
    Amplitudes amplitudes;
    Amplitudes amplitudes_test;

    Analyzer();
    ~Analyzer() override { stop(); }
    void stop() override;

    static std::vector<float> average_channels(const std::vector<float> &left, const std::vector<float> &right);

    void add_samples(const std::vector<float>& left, const std::vector<float> &right, int64_t timestamp_ns = 0);
    void reset_samples();

    void set_update_handler(update_handler_t handler);
    void set_mel_enabled(bool enabled);
    void set_frame_handler(std::function<void(const SpectrumFrame &)> handler);
    static std::vector<float> merge_spectra(const std::vector<float> &left, const std::vector<float> &right, SpectrumMerge mode);
    void set_update_window_function_handler(on_update_window_function_handler_t handler);
    void set_samples_after_window_function_handler(on_samples_after_window_function_handler_t handler);

    void update_sample_rate(size_t sample_rate);

    float get_freq_step();
    std::vector<float> get_samples_snapshot() const;
    std::vector<float> get_window_snapshot() const;

    static void generate_bark_scale(float *scale_ptr, size_t scale_size, float frequency_step);
    static void generate_volume_scale(float *scale_ptr, size_t scale_size, float frequency_step);

    void generate_window();
    void use_window(std::vector<float> &samples_);

};

#endif //QT_VISUALIZATION_ANALYZER_H
