#pragma once
#include <dsp_kernels/prepared_resampler.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

// Benchmark composition only. Platform clocks, heap inspection and scheduling
// stay in the entrypoints; production kernels are linked without modifications.
namespace resampler_packet_benchmark {
using dsp_kernels::PreparedResampler;
using dsp_kernels::ResamplerConfig;
using dsp_kernels::ResamplerMode;
struct Hooks {
    std::uint64_t (*ticks)();
    double ticks_per_us;
    bool wrap32;
    void (*yield)();
    std::size_t (*free_heap)();
    bool (*heap_ok)();
    std::size_t (*allocations)();
    double minimum_batch_us;
};
inline constexpr std::array modes{ResamplerMode::Bypass,          ResamplerMode::ExistingFir,
                                  ResamplerMode::CompactFir,      ResamplerMode::SpeexDsp,
                                  ResamplerMode::IirExperimental, ResamplerMode::LinearDiagnostic};
inline constexpr std::array<std::size_t, 9> sizes{64, 96, 128, 192, 256, 480, 512, 768, 1024};
inline const char *name(ResamplerMode mode) {
    switch (mode) {
    case ResamplerMode::Bypass:
        return "bypass";
    case ResamplerMode::ExistingFir:
        return "existing_fir";
    case ResamplerMode::CompactFir:
        return "compact_fir";
    case ResamplerMode::SpeexDsp:
        return "speex";
    case ResamplerMode::IirExperimental:
        return "iir";
    default:
        return "linear";
    }
}
inline bool success = true;
inline volatile float sink = 0;
inline constexpr float guard = 1234567.f;
inline void check(bool ok, const char *reason) {
    if (!ok) {
        success = false;
        std::printf("FAIL packet_benchmark %s\n", reason);
        std::fflush(stdout);
    }
}
inline double elapsed_us(const Hooks &h, std::uint64_t start, std::uint64_t end) {
    return double(h.wrap32 ? std::uint32_t(end - start) : end - start) / h.ticks_per_us;
}
inline float fixture_sample(std::size_t i, unsigned rate, std::array<float, 7> &re, std::array<float, 7> &im,
                            const std::array<float, 7> &cr, const std::array<float, 7> &ci) {
    float value = 0;
    for (std::size_t k = 0; k < 7; ++k) {
        const float ramp = float((i * (k + 1)) % rate) / float(rate);
        value += (.04f + .02f * ramp) * im[k];
        const float next = re[k] * cr[k] - im[k] * ci[k];
        im[k] = im[k] * cr[k] + re[k] * ci[k];
        re[k] = next;
    }
    return value;
}
inline void fill(std::span<float> values, unsigned rate) {
    // Seven tones with slowly changing amplitudes, no playback. Recurrence
    // avoids repeated transcendental calls in this untimed fixture generator.
    constexpr std::array<float, 7> frequencies{230, 900, 2700, 5300, 9900, 18000, 20500};
    std::array<float, 7> re{}, im{}, cr{}, ci{};
    for (std::size_t k = 0; k < 7; ++k) {
        const float phase = .13f * float(k), step = 6.28318530718f * frequencies[k] / float(rate);
        re[k] = std::cos(phase);
        im[k] = std::sin(phase);
        cr[k] = std::cos(step);
        ci[k] = std::sin(step);
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = fixture_sample(i, rate, re, im, cr, ci);
    }
}
inline std::span<float> interior(std::vector<float> &v) {
    return {v.data() + 1, v.size() - 2};
}
inline bool guarded(const std::vector<float> &v) {
    return v.front() == guard && v.back() == guard;
}

inline double direct_fir_sample(const ResamplerConfig &c, const dsp_kernels::PcmResampler::Geometry &geometry,
                                std::span<const float> weights, std::span<const float> input, std::size_t m) {
    // Independent absolute output index and direct double convolution.
    const auto numerator = std::uint64_t(m) * c.input_rate;
    const auto index = std::size_t((numerator + c.output_rate - 1) / c.output_rate);
    const auto backward = std::uint64_t(index) * c.output_rate - numerator;
    const auto scaled = backward * geometry.phases;
    const auto phase = std::size_t(scaled / c.output_rate);
    const double fraction = double(scaled % c.output_rate) / c.output_rate;
    double expected = 0;
    for (std::size_t k = 0; k < geometry.taps && k <= index; ++k) {
        const double weight =
            (1 - fraction) * weights[phase * geometry.taps + k] + fraction * weights[(phase + 1) * geometry.taps + k];
        expected += double(input[index - k]) * weight;
    }
    return expected;
}
inline std::pair<double, double> direct_fir_errors(const ResamplerConfig &c,
                                                   const dsp_kernels::PcmResampler &coefficients,
                                                   std::span<const float> input, std::span<const float> output,
                                                   std::size_t written) {
    const auto geometry = coefficients.filter_geometry();
    const auto weights = coefficients.coefficients();
    double maximum = 0, square = 0;
    for (std::size_t m = 0; m < written; ++m) {
        const auto expected = direct_fir_sample(c, geometry, weights, input, m);
        const double error = double(output[m]) - expected;
        maximum = std::max(maximum, std::abs(error));
        square += error * error;
    }
    return {maximum, square};
}
inline void validate_fir_double(PreparedResampler &op) {
    const auto c = op.config();
    if (c.mode != ResamplerMode::ExistingFir && c.mode != ResamplerMode::CompactFir)
        return;
    dsp_kernels::PcmResampler coefficients;
    const auto quality = c.mode == ResamplerMode::ExistingFir ? dsp_kernels::PcmResampler::Quality::Full
                                                              : dsp_kernels::PcmResampler::Quality::Compact;
    check(coefficients.prepare(c.input_rate, c.output_rate, 1, quality), "double oracle coefficient preparation");
    std::vector<float> input(2003),
        output(PreparedResampler::maximum_output(input.size(), c.input_rate, c.output_rate));
    fill(input, c.input_rate);
    op.reset();
    std::size_t written = 0;
    check(op.process(input, input.size(), input.size(), output, output.size(), written), "double oracle process");
    const auto [maximum, square] = direct_fir_errors(c, coefficients, input, output, written);
    check(maximum < 2e-6, "FIR direct double arithmetic bound");
    std::printf("{\"check\":\"fir_double_reference\",\"mode\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,\"outputs\":%"
                "zu,\"max_abs\":%.12g,\"rms\":%.12g}\n",
                name(c.mode), unsigned(c.input_rate), unsigned(c.output_rate), written, maximum,
                written ? std::sqrt(square / double(written)) : 0.);
}

struct ToneBasis {
    double frequency = 0, sine = 0, cosine = 1, step_sine = 0, step_cosine = 1, sum_sine = 0, sum_cosine = 0;
};
struct ToneMeasurement {
    static constexpr double pi = 3.14159265358979323846, amplitude = .08;
    static constexpr std::size_t block = 256;
    PreparedResampler &op;
    const Hooks &h;
    ResamplerConfig c;
    double frequency;
    std::span<float> input, output;
    std::array<ToneBasis, 5> bases{};
    std::size_t count, ordinal = 0, measured = 0;
    unsigned skip, total;
    ToneMeasurement(PreparedResampler &operation, const Hooks &hooks, double f, std::span<float> in,
                    std::span<float> out)
        : op(operation), h(hooks), c(op.config()), frequency(f), input(in), output(out), count(f == 5300. ? 5 : 1),
          skip(c.output_rate / 4), total(c.input_rate + c.input_rate / 4) {}
    void initialize() {
        const auto fold = [&](double f) { return std::abs(f - c.output_rate * std::round(f / c.output_rate)); };
        const double delta = std::abs(double(c.input_rate) - c.output_rate);
        const std::array<double, 5> targets{fold(frequency), 10600., 15900., fold(frequency + delta),
                                            fold(frequency - delta)};
        for (std::size_t j = 0; j < count; ++j) {
            auto &basis = bases[j];
            basis.frequency = targets[j];
            const auto step = 2 * pi * basis.frequency / c.output_rate, initial = step * skip;
            basis.sine = std::sin(initial);
            basis.cosine = std::cos(initial);
            basis.step_sine = std::sin(step);
            basis.step_cosine = std::cos(step);
        }
    }
    void record(double value) {
        ++measured;
        for (std::size_t j = 0; j < count; ++j) {
            auto &basis = bases[j];
            basis.sum_sine += value * basis.sine;
            basis.sum_cosine += value * basis.cosine;
            const double next = basis.sine * basis.step_cosine + basis.cosine * basis.step_sine;
            basis.cosine = basis.cosine * basis.step_cosine - basis.sine * basis.step_sine;
            basis.sine = next;
        }
    }
    void fill_packet(std::size_t at, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            input[i] = float(amplitude * std::sin(2 * pi * frequency * double((at + i) % c.input_rate) / c.input_rate));
    }
    void record_packet(std::size_t written) {
        for (std::size_t i = 0; i < written; ++i, ++ordinal)
            if (ordinal >= skip)
                record(output[i]);
    }
    void stream() {
        for (std::size_t at = 0, packet_index = 0; at < total; ++packet_index) {
            const auto n = std::min<std::size_t>(block, total - at);
            fill_packet(at, n);
            std::size_t written = 0;
            check(op.process(input, n, n, output, output.size(), written), "targeted tone process");
            record_packet(written);
            at += n;
            if ((packet_index & 15) == 15)
                h.yield();
        }
    }
    void report(std::size_t allocations) {
        check(measured == c.output_rate && ordinal == c.output_rate + c.output_rate / 4,
              "one-second coherent hardware tone count");
        check(h.allocations() == allocations, "targeted tones no vendor hot allocations");
        const double primary = 2 * std::hypot(bases[0].sum_sine, bases[0].sum_cosine) / double(measured);
        std::printf(
            "{\"check\":\"tone_quality\",\"mode\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,\"frequency_hz\":%.0f,"
            "\"folded_hz\":%.0f,\"main_gain_db\":%.9g,\"measurement_samples\":%zu,\"settling_samples\":%u,\"lines\":[",
            name(c.mode), unsigned(c.input_rate), unsigned(c.output_rate), frequency, bases[0].frequency,
            20 * std::log10(std::max(primary / amplitude, 1e-30)), measured, unsigned(skip));
        const char *labels[] = {"main", "harmonic2", "harmonic3", "image_plus", "image_minus"};
        for (std::size_t j = 1; j < count; ++j) {
            const double value = 2 * std::hypot(bases[j].sum_sine, bases[j].sum_cosine) / double(measured);
            std::printf("%s{\"kind\":\"%s\",\"frequency_hz\":%.0f,\"level_dbc\":%.9g}", j > 1 ? "," : "", labels[j],
                        bases[j].frequency, 20 * std::log10(std::max(value / std::max(primary, 1e-30), 1e-30)));
        }
        std::printf("]}\n");
        std::fflush(stdout);
    }
    void run() {
        initialize();
        op.reset();
        const auto allocations = h.allocations();
        stream();
        report(allocations);
    }
};
inline void validate_tones(PreparedResampler &op, const Hooks &h) {
    const auto c = op.config();
    if (c.mode == ResamplerMode::Bypass)
        return;
    std::vector<float> input(ToneMeasurement::block),
        output(PreparedResampler::maximum_output(ToneMeasurement::block, c.input_rate, c.output_rate));
    for (double frequency : {5300., 9900., 16000., 20500.}) {
        ToneMeasurement measurement(op, h, frequency, input, output);
        measurement.run();
    }
    check(h.heap_ok(), "targeted tones heap integrity");
}

struct StreamingValidation {
    static constexpr std::size_t total = 16384;
    PreparedResampler &op;
    const Hooks &h;
    ResamplerConfig c;
    std::span<float> input, whole, fragment;
    std::size_t expected;
    void compare_fragment(std::size_t written, std::size_t count, double &maximum, double &square,
                          bool &same_bits) const {
        for (std::size_t j = 0; j < written && count + j < expected; ++j) {
            const double error = double(fragment[j]) - whole[count + j];
            maximum = std::max(maximum, std::abs(error));
            square += error * error;
            same_bits = same_bits &&
                        std::bit_cast<std::uint32_t>(fragment[j]) == std::bit_cast<std::uint32_t>(whole[count + j]);
        }
    }
    void report_partition(std::size_t packet, std::size_t count, double maximum, double square,
                          bool same_bits, std::size_t allocations) const {
        check(count == expected && maximum == 0 && same_bits, "exact packet partition invariance");
        check(h.allocations() == allocations, "validation no hot allocations");
        std::printf("{\"check\":\"stream_partition\",\"mode\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,\"input_"
                    "samples\":%zu,\"packet\":%zu,\"reference_outputs\":%zu,\"outputs\":%zu,\"max_abs\":%.9g,\"rms\":%."
                    "9g,\"bitwise_equal\":%s}\n",
                    name(c.mode), unsigned(c.input_rate), unsigned(c.output_rate), total, packet, expected, count,
                    maximum, count ? std::sqrt(square / double(count)) : 0., same_bits ? "true" : "false");
    }
    void partition(std::size_t packet) {
        op.reset();
        std::size_t count = 0;
        double maximum = 0, square = 0;
        bool same_bits = true;
        const auto allocations = h.allocations();
        for (std::size_t at = 0, step = 0; at < total; ++step) {
            const auto n = std::min(total - at, packet ? packet : 1 + (step * 37) % 1024);
            std::size_t written = 0;
            check(op.process(input.subspan(at, n), n, n, fragment, fragment.size(), written),
                  "stream validation process");
            check(count + written <= expected, "stream count bounded");
            compare_fragment(written, count, maximum, square, same_bits);
            count += written;
            at += n;
            if ((step & 15) == 15)
                h.yield();
        }
        report_partition(packet, count, maximum, square, same_bits, allocations);
    }
    void negative_control() {
        // Deliberately wrong use: a fresh history AND phase at every packet.
        // A count mismatch means different grids; prefix error is diagnostic only.
        std::size_t count = 0;
        double prefix_maximum = 0;
        for (std::size_t at = 0; at < total; at += 256) {
            op.reset();
            std::size_t written = 0;
            check(op.process(input.subspan(at, 256), 256, 256, fragment, fragment.size(), written),
                  "reset negative control process");
            for (std::size_t j = 0; j < written && count + j < expected; ++j)
                prefix_maximum = std::max(prefix_maximum, std::abs(double(fragment[j]) - whole[count + j]));
            count += written;
            if ((at % 4096) == 3840)
                h.yield();
        }
        std::printf("{\"check\":\"reset_negative_control\",\"mode\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,"
                    "\"packet\":256,\"reference_outputs\":%zu,\"outputs\":%zu,\"same_grid_count\":%s,\"prefix_max_abs_"
                    "diagnostic_only\":%.9g}\n",
                    name(c.mode), unsigned(c.input_rate), unsigned(c.output_rate), expected, count,
                    count == expected ? "true" : "false", prefix_maximum);
    }
};
inline std::size_t prepare_stream_reference(PreparedResampler &op, std::span<float> input, std::span<float> whole,
                                            std::uint32_t input_rate, std::size_t total) {
    fill(input, input_rate);
    op.reset();
    std::size_t expected = 0;
    check(op.process(input, total, total, whole, whole.size(), expected), "whole validation process");
    for (std::size_t i = 0; i < expected; ++i)
        check(std::isfinite(whole[i]), "finite whole output");
    return expected;
}
inline void validate_streaming(PreparedResampler &op, const Hooks &h) {
    const auto c = op.config();
    constexpr std::size_t total = 16384;
    const auto whole_capacity = PreparedResampler::maximum_output(total, c.input_rate, c.output_rate);
    const auto fragment_capacity = PreparedResampler::maximum_output(sizes.back(), c.input_rate, c.output_rate);
    std::vector<float> input_storage(total + 2, guard), whole_storage(whole_capacity + 2, guard),
        fragment_storage(fragment_capacity + 2, guard);
    auto input = interior(input_storage), whole = interior(whole_storage), fragment = interior(fragment_storage);
    const auto expected = prepare_stream_reference(op, input, whole, c.input_rate, total);
    StreamingValidation validation{op, h, c, input, whole, fragment, expected};
    for (const std::size_t packet : std::array<std::size_t, 10>{64, 96, 128, 192, 256, 480, 512, 768, 1024, 0})
        validation.partition(packet);
    validation.negative_control();
    check(guarded(input_storage) && guarded(whole_storage) && guarded(fragment_storage), "validation canaries");
    check(h.heap_ok(), "validation heap integrity");
}

template <class Process> inline double calibration_trials(Process &process, const Hooks &h, std::size_t batch) {
    double fastest = 1e100;
    for (unsigned trial = 0; trial < 3; ++trial) {
        h.yield();
        const auto begin = h.ticks();
        bool ok = true;
        for (std::size_t j = 0; j < batch; ++j)
            ok = process().first && ok;
        fastest = std::min(fastest, elapsed_us(h, begin, h.ticks()));
        check(ok, "batch calibration");
    }
    return fastest;
}
template <class Process> inline std::size_t calibrate_batch(Process &process, const Hooks &h) {
    std::size_t batch = 1;
    if (h.minimum_batch_us > 0) {
        for (;;) {
            double fastest = 1e100;
            fastest = calibration_trials(process, h, batch);
            if (fastest >= 2 * h.minimum_batch_us || batch >= 262144)
                break;
            batch *= 2;
        }
    }
    return batch;
}
template <class Process>
inline void measure_packet_set(Process &process, const Hooks &h, std::size_t batch, std::array<double, 33> &durations,
                               std::size_t &total_outputs, double &minimum_batch) {
    for (auto &duration : durations) {
        h.yield();
        const auto begin = h.ticks();
        bool ok = true;
        std::size_t n = 0;
        for (std::size_t j = 0; j < batch; ++j) {
            const auto result = process();
            ok = result.first && ok;
            n += result.second;
        }
        const auto us = elapsed_us(h, begin, h.ticks());
        duration = us / double(batch);
        minimum_batch = std::min(minimum_batch, us);
        check(ok, "timed process");
        total_outputs += n;
    }
}
template <class Process>
inline unsigned measure_packet_batches(Process &process, const Hooks &h, std::size_t &batch,
                                       std::array<double, 33> &durations, std::size_t &total_outputs,
                                       double &minimum_batch) {
    unsigned measurement_sets = 0;
    for (;;) {
        ++measurement_sets;
        total_outputs = 0;
        minimum_batch = 1e100;
        measure_packet_set(process, h, batch, durations, total_outputs, minimum_batch);
        if (h.minimum_batch_us == 0 || minimum_batch >= h.minimum_batch_us || batch >= 262144)
            break;
        // Adapt only to timer granularity, never discard a slow sample.
        batch *= 2;
    }
    return measurement_sets;
}
struct PacketReport {
    PreparedResampler &op;
    std::size_t packet, capacity, batch;
    unsigned measurement_sets;
    std::size_t total_outputs, kernel_heap_delta, staging_heap_delta;
    double minimum_batch;
    const std::array<double, 33> &durations;
    void print() const {
        const auto c = op.config();
        constexpr std::size_t samples = 33;
        const auto r = op.resources();
        std::printf(
            "{\"method\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,\"input_packet\":%zu,\"output_capacity\":%zu,"
            "\"samples\":%zu,\"batch_calls\":%zu,\"measurement_sets\":%u,\"minimum_batch_us\":%.6f,\"output_samples_"
            "total\":%zu,\"owned_payload_bytes\":%zu,\"kernel_heap_delta_bytes\":%zu,\"packet_payload_bytes\":%zu,"
            "\"guard_bytes\":16,\"staging_heap_delta_bytes\":%zu,\"p50_us\":%.6f,\"p95_us\":%.6f,\"max_us\":%.6f}\n",
            name(c.mode), unsigned(c.input_rate), unsigned(c.output_rate), packet, capacity, samples, batch,
            measurement_sets, minimum_batch, total_outputs, r.owned_bytes, kernel_heap_delta,
            (packet + capacity) * sizeof(float), staging_heap_delta, durations[samples / 2],
            durations[(samples * 95 + 99) / 100 - 1], durations.back());
        std::fflush(stdout);
    }
};
inline void finish_packet_timing(PreparedResampler &op, std::size_t packet, const Hooks &h,
                                 std::size_t kernel_heap_delta, std::size_t staging_heap_delta, std::size_t capacity,
                                 std::size_t batch, unsigned measurement_sets, double minimum_batch,
                                 std::size_t total_outputs, std::size_t allocations, std::size_t heap,
                                 std::array<double, 33> &durations, const std::vector<float> &input_storage,
                                 const std::vector<float> &output_storage) {
    check(h.allocations() == allocations && h.free_heap() == heap, "timing no hot allocations");
    check(h.minimum_batch_us == 0 || minimum_batch >= h.minimum_batch_us, "PC batch duration target");
    check(guarded(input_storage) && guarded(output_storage), "timing canaries");
    check(h.heap_ok(), "timing heap integrity");
    std::sort(durations.begin(), durations.end());
    PacketReport{op,
                 packet,
                 capacity,
                 batch,
                 measurement_sets,
                 total_outputs,
                 kernel_heap_delta,
                 staging_heap_delta,
                 minimum_batch,
                 durations}
        .print();
}
template <class Process>
inline void measure_staged_packet(PreparedResampler &op, std::size_t packet, const Hooks &h,
                                  std::size_t kernel_heap_delta, std::size_t staging_heap_delta, std::size_t capacity,
                                  Process &process, const std::vector<float> &input_storage,
                                  const std::vector<float> &output_storage) {
    for (unsigned i = 0; i < 3; ++i) {
        check(process().first, "timing warmup");
        h.yield();
    }
    auto batch = calibrate_batch(process, h);
    std::array<double, 33> durations{};
    std::size_t total_outputs = 0;
    double minimum_batch = 1e100;
    const auto allocations = h.allocations(), heap = h.free_heap();
    const auto measurement_sets = measure_packet_batches(process, h, batch, durations, total_outputs, minimum_batch);
    finish_packet_timing(op, packet, h, kernel_heap_delta, staging_heap_delta, capacity, batch, measurement_sets,
                         minimum_batch, total_outputs, allocations, heap, durations, input_storage, output_storage);
}
inline void time_packet(PreparedResampler &op, std::size_t packet, const Hooks &h, std::size_t kernel_heap_delta) {
    const auto c = op.config();
    const auto capacity = PreparedResampler::maximum_output(packet, c.input_rate, c.output_rate);
    const auto heap_before = h.free_heap();
    std::vector<float> input_storage(packet + 2, guard), output_storage(capacity + 2, guard);
    auto input = interior(input_storage), output = interior(output_storage);
    fill(input, c.input_rate);
    const auto staging_heap_delta = heap_before - h.free_heap();
    op.reset();
    auto process = [&]() {
        std::size_t n = 0;
        const bool ok = op.process(input, packet, packet, output, capacity, n);
        if (n)
            sink = output[n - 1];
        return std::pair{ok, n};
    };
    measure_staged_packet(op, packet, h, kernel_heap_delta, staging_heap_delta, capacity, process,
                          input_storage, output_storage);
}

inline void report_mode_resources(const PreparedResampler &op, const ResamplerConfig &c,
                                  const dsp_kernels::ResamplerResources &query, std::size_t kernel_heap_delta,
                                  double prepare_us) {
    const auto r = op.resources();
    check(r.owned_bytes == query.owned_bytes && r.coefficient_bytes == query.coefficient_bytes &&
              r.state_bytes == query.state_bytes,
          "resource ledger");
    std::printf("{\"check\":\"resources\",\"mode\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,\"owned_payload_bytes\":"
                "%zu,\"coefficient_bytes\":%zu,\"state_bytes\":%zu,\"object_bytes\":%zu,\"kernel_heap_delta_bytes\":%"
                "zu,\"prepare_us\":%.3f,\"delay_input_samples\":%u,\"nonlinear_delay\":%s,\"alias_warning\":%s}\n",
                name(c.mode), unsigned(c.input_rate), unsigned(c.output_rate), r.owned_bytes, r.coefficient_bytes,
                r.state_bytes, sizeof(PreparedResampler), kernel_heap_delta, prepare_us,
                unsigned(r.fixed_delay_input_samples), r.frequency_dependent_delay ? "true" : "false",
                r.alias_warning ? "true" : "false");
    std::fflush(stdout);
}
inline void one_mode(ResamplerConfig c, const dsp_kernels::ResamplerBackendFactory &factory, const Hooks &h) {
    dsp_kernels::ResamplerResources query;
    check(PreparedResampler::resource_requirements(c, &factory, query), "resource query");
    const auto heap_before = h.free_heap();
    {
        PreparedResampler op;
        const auto start = h.ticks();
        check(op.prepare(c, &factory), "prepare mode");
        const auto prepare_us = elapsed_us(h, start, h.ticks());
        const auto kernel_heap_delta = heap_before - h.free_heap();
        report_mode_resources(op, c, query, kernel_heap_delta, prepare_us);
        validate_streaming(op, h);
        for (const auto packet : sizes) {
            time_packet(op, packet, h, kernel_heap_delta);
            h.yield();
        }
        validate_fir_double(op);
        validate_tones(op, h);
    }
    check(h.free_heap() == heap_before && h.heap_ok(), "mode cleanup heap restored");
}
inline bool run(const Hooks &h, const dsp_kernels::ResamplerBackendFactory &factory) {
    success = true;
    for (const unsigned input : {48000u, 44100u}) {
        one_mode({ResamplerMode::Bypass, input, input, 1, 5}, factory, h);
        for (const unsigned output : {36000u})
            for (const auto mode : modes) {
                if (mode != ResamplerMode::Bypass)
                    one_mode({mode, input, output, 1, 5}, factory, h);
            }
    }
    return success;
}
} // namespace resampler_packet_benchmark
