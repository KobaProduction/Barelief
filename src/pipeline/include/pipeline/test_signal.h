#pragma once
#include "pipeline/audio_source.h"

namespace pipeline {
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
