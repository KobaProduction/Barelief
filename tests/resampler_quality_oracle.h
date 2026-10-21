#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>
#include <set>
#include <stdexcept>
#include <vector>

namespace resampler_test_oracle {
inline constexpr double pi = std::numbers::pi, amplitude = .08;
struct Tone {
    double frequency, phase, sine = 0, cosine = 0;
};
struct Fit {
    std::vector<Tone> tones;
    double residual = 0, fitted = 0, dc = 0;
};
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline double fold(double frequency, unsigned rate) {
    return std::abs(frequency - rate * std::round(frequency / rate));
}
inline Tone measure_tone(const std::vector<float> &output, unsigned rate, double frequency, double phase) {
    Tone tone{frequency, phase};
    for (std::size_t i = rate; i < 2 * rate; ++i) {
        const auto angle = 2 * pi * frequency * double(i) / rate;
        tone.sine += double(output[i]) * std::sin(angle);
        tone.cosine += double(output[i]) * std::cos(angle);
    }
    tone.sine *= 2. / rate;
    tone.cosine *= 2. / rate;
    return tone;
}
inline void append_tones(Fit &result, std::set<double> &aliases, const std::vector<float> &output, unsigned rate,
                         const std::vector<double> &frequencies, double phase_offset) {
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        const auto frequency = fold(frequencies[k], rate);
        require(frequency > 0 && frequency < rate / 2. && frequency == std::floor(frequency) &&
                    aliases.insert(frequency).second,
                "Nonorthogonal tone probe");
        const auto tone = measure_tone(output, rate, frequency, .13 * k + phase_offset);
        result.fitted += (tone.sine * tone.sine + tone.cosine * tone.cosine) / 2;
        result.tones.push_back(tone);
    }
}
inline double measure_dc(const std::vector<float> &output, unsigned rate) {
    double dc = 0;
    for (std::size_t i = rate; i < 2 * rate; ++i)
        dc += output[i];
    return dc / rate;
}
inline double sample_residual(const Fit &fit, const std::vector<float> &output, unsigned rate, std::size_t i) {
    double residual = output[i] - fit.dc;
    for (const auto &tone : fit.tones) {
        const auto angle = 2 * pi * tone.frequency * double(i) / rate;
        residual -= tone.sine * std::sin(angle) + tone.cosine * std::cos(angle);
    }
    return residual;
}
// The one-second integer-Hz bases are orthogonal. Phase is measured without
// inventing a fixed IIR delay; direct residual avoids subtracting close powers.
inline Fit fit(const std::vector<float> &output, unsigned rate, const std::vector<double> &frequencies,
               double phase_offset = 0) {
    require(output.size() >= 2 * rate, "Insufficient settling/measurement duration");
    Fit result;
    std::set<double> aliases;
    append_tones(result, aliases, output, rate, frequencies, phase_offset);
    result.dc = measure_dc(output, rate);
    for (std::size_t i = rate; i < 2 * rate; ++i) {
        const auto residual = sample_residual(result, output, rate, i);
        result.residual += residual * residual;
    }
    result.residual = std::sqrt(result.residual / rate);
    result.fitted = std::sqrt(result.fitted);
    return result;
}
} // namespace resampler_test_oracle
