#pragma once
#include "pipeline/audio_source.h"

namespace pipeline {
// Checked absolute sample clock conversion, shared by source and long-run regression.
bool test_signal_sample_time_ns(std::uint64_t samples,std::uint32_t rate,std::int64_t origin_ns,std::int64_t& timestamp_ns) noexcept;
struct TestSignalSettings {
    int tones = 8, sample_rate = 48000;
    double minimum_hz = 73, maximum_hz = 13901, amplitude = 0.4;
    double period_seconds = 6, sweep_depth = 0.18;
    QJsonObject json() const;
    static bool parse(const QJsonObject &json, TestSignalSettings &settings, QString &error);
    bool operator==(const TestSignalSettings &) const = default;
};
std::unique_ptr<IAudioSource> make_test_signal_source();
}
