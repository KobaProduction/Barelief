#include <QCoreApplication>
#include <QFile>
#include <QDebug>
#include <miniaudio.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <array>
#include <cmath>

namespace {
uint32_t le32(const QByteArray &bytes, int offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("Truncated WAV header");
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}
uint16_t le16(const QByteArray &bytes, int offset) {
    if (offset + 2 > bytes.size()) throw std::runtime_error("Truncated PCM sample/header");
    return uint16_t(static_cast<unsigned char>(bytes[offset])) |
        (uint16_t(static_cast<unsigned char>(bytes[offset + 1])) << 8);
}
// Fixture loader only: production miniaudio remains built without file codecs.
std::vector<float> load_fixture(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
        throw std::runtime_error("Cannot open WAV fixture (limit 64 MiB)");
    const auto header = file.read(12);
    if (header.size() != 12 || header.first(4) != "RIFF" || header.mid(8, 4) != "WAVE")
        throw std::runtime_error("Fixture must be RIFF/WAVE");
    QByteArray format, pcm;
    while (!file.atEnd()) {
        const auto chunk = file.read(8);
        if (chunk.size() != 8) throw std::runtime_error("Truncated WAV chunk");
        const auto size = le32(chunk, 4);
        if (qint64(size) > file.size() - file.pos()) throw std::runtime_error("Invalid WAV chunk size");
        const auto bytes = file.read(size);
        if (chunk.first(4) == "fmt ") format = bytes;
        if (chunk.first(4) == "data") pcm = bytes;
        if (size & 1) file.read(1);
    }
    if (format.size() < 16 || le16(format, 0) != 1 || le16(format, 2) != 2 ||
        le32(format, 4) != 48000 || le16(format, 12) != 4 || le16(format, 14) != 16 || pcm.isEmpty() || pcm.size() % 4)
        throw std::runtime_error("Fixture must be 48 kHz stereo PCM16");
    std::vector<float> samples(pcm.size() / 2);
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto raw = le16(pcm, int(i * 2));
        const auto signed_value = raw < 32768 ? int(raw) : int(raw) - 65536;
        samples[i] = signed_value / 32768.0f * 0.01f;
    }
    return samples;
}
struct Playback {
    std::vector<float> samples;
    size_t position = 0;
    std::atomic<uint64_t> loops{0};
    ma_device device{};
    bool initialized = false;
    ~Playback() { if (initialized) ma_device_uninit(&device); }
    static void callback(ma_device *device, void *output, const void *, ma_uint32 frames) noexcept {
        auto &self = *static_cast<Playback *>(device->pUserData);
        auto *destination = static_cast<float *>(output);
        while (frames) {
            const auto count = std::min<size_t>(frames, self.samples.size() / 2 - self.position);
            std::memcpy(destination, self.samples.data() + self.position * 2, count * 2 * sizeof(float));
            destination += count * 2; frames -= count; self.position += count;
            if (self.position == self.samples.size() / 2) { self.position = 0; ++self.loops; }
        }
    }
};
std::vector<float> changing_tones() {
    constexpr std::array<double, 8> frequencies{73, 211, 503, 997, 2311, 4903, 7919, 13901};
    constexpr size_t frames = 6 * 48000;
    constexpr double tau = 6.283185307179586;
    std::vector<float> result(frames * 2);
    for (size_t frame = 0; frame < frames; ++frame) {
        const double time = frame / 48000.0;
        double left = 0, right = 0;
        for (size_t tone = 0; tone < frequencies.size(); ++tone) {
            const double sweep = tau * (tone + 1) / 6;
            const double phase = tau * (frequencies[tone] * time +
                0.18 * frequencies[tone] * (1 - std::cos(sweep * time)) / sweep);
            const double gain = 0.005 * (0.65 + 0.35 * std::sin(tau * (tone + 2) * time / 6));
            left += gain * std::sin(phase);
            right += gain * std::sin(phase + (tone + 1) * 0.31);
        }
        result[frame * 2] = float(left); result[frame * 2 + 1] = float(right);
    }
    // Integer carriers and six-second modulation periods make the loop seam
    // phase-continuous. All synthesis/allocations happen before device start.
    return result;
}
}
int main(int argc, char **argv) {
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() != 3) throw std::runtime_error("Usage: native_music_player fixture.wav duration_seconds");
        bool ok = false;
        const auto seconds = app.arguments()[2].toInt(&ok);
        if (!ok || seconds < 1 || seconds > 3600) throw std::runtime_error("Duration must be 1–3600 seconds");
        const bool tones = app.arguments()[1] == "--tones";
        Playback playback; playback.samples = tones ? changing_tones() : load_fixture(app.arguments()[1]);
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32; config.playback.channels = 2; config.sampleRate = 48000;
        config.pUserData = &playback; config.dataCallback = Playback::callback;
        auto result = ma_device_init(nullptr, &config, &playback.device);
        if (result != MA_SUCCESS) throw std::runtime_error(ma_result_description(result));
        playback.initialized = true;
        result = ma_device_start(&playback.device);
        if (result != MA_SUCCESS) throw std::runtime_error(ma_result_description(result));
        qInfo("AUDIO_READY signal=%s duration_s=%.3f backend=%s", tones ? "8-changing-sinusoids" : "wav",
            playback.samples.size() / 96000.0,
            ma_get_backend_name(playback.device.pContext->backend));
        for (int elapsed = 0; elapsed < seconds; ++elapsed) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (elapsed % 60 == 59) qInfo("AUDIO_PLAYING elapsed_s=%d loops=%llu", elapsed + 1,
                static_cast<unsigned long long>(playback.loops.load()));
        }
        qInfo("AUDIO_STOPPED"); return 0;
    } catch (const std::exception &error) { qCritical("Music fixture: %s", error.what()); return 1; }
}
