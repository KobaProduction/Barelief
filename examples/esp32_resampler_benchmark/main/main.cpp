#include <dsp_kernels/pcm_resampler.h>
#include <dsp_kernels/prepared_resampler.h>
#include <speex_backend.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "corrected_double_reference.h"
#include "dot_probe.h"
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using dsp_kernels::PcmResampler;
using dsp_kernels::PreparedResampler;
using dsp_kernels::ResamplerMode;
extern "C" std::size_t qtvis_speex_allocation_calls();
constexpr float guard = 1234567.f;
bool success = true;
volatile float sink = 0;
void check(bool value, const char *reason) {
    if (!value) {
        std::printf("FAIL resampler %s\n", reason);
        success = false;
    }
}
void fill(std::span<float> input, unsigned rate, std::size_t offset) {
    constexpr float frequencies[]{230, 900, 2700, 5300, 9900, 18000, 20500};
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float t = float(offset + i) / float(rate);
        float value = 0;
        for (unsigned k = 0; k < 7; ++k)
            value +=
                (.05f + .01f * std::sin((k + 1) * .7f * t)) * std::sin(6.28318530718f * frequencies[k] * t + .13f * k);
        input[i] = value;
    }
}
template <class Process> std::size_t measure_samples(std::array<std::uint32_t, 33> &durations, Process &process) {
    std::size_t total_outputs = 0, outputs = 0;
    constexpr unsigned n = 33;
    for (unsigned k = 0; k < n; ++k) {
        vTaskDelay(1);
        const auto start = esp_cpu_get_cycle_count();
        const bool ok = process(outputs);
        durations[k] = esp_cpu_get_cycle_count() - start;
        check(ok, "timed process");
        total_outputs += outputs;
    }
    return total_outputs;
}
template <class Process>
void measure(const char *method, unsigned in, unsigned out, std::size_t packet, std::size_t bytes, Process process) {
    std::array<std::uint32_t, 33> durations{};
    std::size_t outputs = 0;
    const auto begin = esp_cpu_get_cycle_count();
    check(process(outputs), "pilot");
    const auto pilot = esp_cpu_get_cycle_count() - begin;
    (void)pilot;
    constexpr unsigned n = 33;
    for (unsigned k = 0; k < 3; ++k) {
        check(process(outputs), "warmup");
        vTaskDelay(1);
    }
    std::size_t total_outputs = 0;
    total_outputs = measure_samples(durations, process);
    std::sort(durations.begin(), durations.begin() + n);
    const double mhz = esp_rom_get_cpu_ticks_per_us();
    std::printf("{\"method\":\"%s\",\"input_rate\":%u,\"output_rate\":%u,\"input_packet\":%zu,\"samples\":%u,\"output_"
                "samples\":%zu,\"owned_payload_bytes\":%zu,\"p50_cycles\":%lu,\"p95_cycles\":%lu,\"max_cycles\":%lu,"
                "\"p50_us\":%.3f,\"p95_us\":%.3f,\"max_us\":%.3f}\n",
                method, in, out, packet, n, total_outputs, bytes, (unsigned long)durations[n / 2],
                (unsigned long)durations[(n * 95 + 99) / 100 - 1], (unsigned long)durations[n - 1],
                durations[n / 2] / mhz, durations[(n * 95 + 99) / 100 - 1] / mhz, durations[n - 1] / mhz);
    std::fflush(stdout);
}
double compare_fir_blocks(PcmResampler &current, CorrectedDoubleReference &oracle, unsigned in,
                          std::span<float> input, std::span<float> a, std::span<float> b) {
    double maximum = 0;
    for (unsigned block = 0; block < 2; ++block) {
        fill(input, in, block * input.size());
        std::size_t na = 0, nb = 0;
        check(current.process(input, input.size(), input.size(), a, a.size(), na), "Float32 numeric");
        check(oracle.process(input, b, nb), "double numeric");
        check(na == nb, "numeric count");
        for (std::size_t i = 0; i < std::min(na, nb); ++i) {
            check(std::isfinite(a[i]), "finite output");
            maximum = std::max(maximum, std::abs(double(a[i]) - b[i]));
        }
        vTaskDelay(1);
    }
    return maximum;
}
void report_fir_accuracy(PcmResampler &current, PcmResampler::Quality quality, unsigned in, unsigned out,
                         std::size_t packet, double maximum) {
    check(maximum < 2e-6, "Float32/double tolerance");
    const char *mode = quality == PcmResampler::Quality::Full ? "full" : "compact";
    std::printf("{\"check\":\"float_vs_double\",\"mode\":\"%s\",\"in\":%u,\"out\":%u,\"packet\":%zu,\"max_abs\":%."
                "9g,\"delay_input_samples\":%u,\"taps\":%zu,\"phases\":%zu}\n",
                mode, in, out, packet, maximum, unsigned(current.delay_input_samples()),
                current.filter_geometry().taps, current.filter_geometry().phases);
    check(heap_caps_check_integrity_all(true), "heap integrity after quality");
}
void fir_quality(unsigned in, unsigned out, PcmResampler::Quality quality, std::span<float> input,
                 std::span<float> a, std::span<float> b) {
    PcmResampler current;
    const auto prepare_start = esp_timer_get_time();
    check(current.prepare(in, out, 1, quality), "prepare Float32");
    const auto prepare_us = esp_timer_get_time() - prepare_start;
    std::printf("{\"check\":\"cold_prepare\",\"mode\":\"%s\",\"in\":%u,\"out\":%u,\"packet\":%zu,\"prepare_us\":%lld}\n",
                quality == PcmResampler::Quality::Full ? "full" : "compact", in, out, input.size(), (long long)prepare_us);
    CorrectedDoubleReference oracle(current, in, out);
    const double maximum = compare_fir_blocks(current, oracle, in, input, a, b);
    report_fir_accuracy(current, quality, in, out, input.size(), maximum);
}
void fir_oracle(unsigned in, unsigned out) {
    constexpr std::size_t packet = 1021;
    const auto capacity = PcmResampler::maximum_output(packet, in, out);
    std::vector<float> input_storage(packet + 2, guard), a_storage(capacity + 2, guard), b_storage(capacity + 2, guard);
    auto input = std::span(input_storage).subspan(1, packet), a = std::span(a_storage).subspan(1, capacity),
         b = std::span(b_storage).subspan(1, capacity);
    for (const auto quality : {PcmResampler::Quality::Full, PcmResampler::Quality::Compact})
        fir_quality(in, out, quality, input, a, b);
    check(input_storage.front() == guard && input_storage.back() == guard && a_storage.front() == guard &&
              a_storage.back() == guard && b_storage.front() == guard && b_storage.back() == guard,
          "buffer canaries");
}
const char *name(ResamplerMode mode) {
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
void reject_nonfinite(PreparedResampler &identity, std::array<float, 8> &input, std::array<float, 9> &output) {
    for (const auto invalid : {0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u}) {
        input.back() = std::bit_cast<float>(invalid);
        output.fill(42.f);
        const auto produced = identity.produced();
        std::size_t n = 99;
        check(!identity.process(input, input.size(), input.size(), output, output.size(), n) && n == 0 &&
                  identity.produced() == produced,
              "NaN/Inf atomic rejection");
        for (const auto value : output)
            check(value == 42.f, "NaN/Inf leaves output unchanged");
    }
}
void finite_mode(ResamplerMode mode) {
    PreparedResampler identity;
    check(identity.prepare({mode, 48000, 48000, 1, 5}, &desktop_resampling::speex_backend_factory()),
          "finite identity prepare");
    const std::array<std::uint32_t, 8> patterns{0u,          0x80000000u, 1u,          0x80000001u,
                                                0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0xbf800000u};
    std::array<float, 8> input{};
    std::array<float, 9> output{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::bit_cast<float>(patterns[i]);
    std::size_t n = 0;
    check(identity.process(input, input.size(), input.size(), output, output.size(), n) && n == input.size(),
          "finite identity process");
    for (std::size_t i = 0; i < input.size(); ++i)
        check(std::bit_cast<std::uint32_t>(output[i]) == patterns[i], "finite bit patterns preserved");
    reject_nonfinite(identity, input, output);
}
void finite_classification() {
    for (const auto mode : {ResamplerMode::Bypass, ResamplerMode::ExistingFir, ResamplerMode::CompactFir,
                            ResamplerMode::SpeexDsp, ResamplerMode::IirExperimental, ResamplerMode::LinearDiagnostic})
        finite_mode(mode);
    std::printf("{\"check\":\"finite_classification\",\"modes\":6,\"patterns\":12}\n");
}
struct ModeBuffers {
    std::vector<float> output_storage;
    std::vector<float> whole;
    std::vector<float> fragment;
    std::span<float> output;
    ModeBuffers(std::size_t capacity, std::size_t fragment_capacity)
        : output_storage(capacity + 2, guard), whole(capacity * 4), fragment(fragment_capacity),
          output(std::span(output_storage).subspan(1, capacity)) {}
};
long long prepare_mode(PreparedResampler &op, const dsp_kernels::ResamplerConfig &config,
                       dsp_kernels::ResamplerResources &query) {
    const auto &factory = desktop_resampling::speex_backend_factory();
    check(PreparedResampler::resource_requirements(config, &factory, query), "resource query");
    const auto prepare_start = esp_timer_get_time();
    check(op.prepare(config, &factory), "prepare mode");
    return esp_timer_get_time() - prepare_start;
}
void report_mode(PreparedResampler &op, const dsp_kernels::ResamplerResources &query, ResamplerMode mode,
                  unsigned in, unsigned out, std::size_t packet, long long prepare_us) {
    const auto r = op.resources();
    check(query.owned_bytes == r.owned_bytes, "resource ledger matches prepared allocations");
    std::printf("{\"check\":\"mode_resources\",\"mode\":\"%s\",\"in\":%u,\"out\":%u,\"packet\":%zu,\"quality\":5,"
                "\"prepare_us\":%lld,\"owned_payload_bytes\":%zu,\"coefficient_bytes\":%zu,\"state_bytes\":%zu,"
                "\"fixed_delay_input_samples\":%u,\"nonlinear_delay\":%s,\"alias_warning\":%s}\n",
                name(mode), in, out, packet, (long long)prepare_us, r.owned_bytes, r.coefficient_bytes,
                r.state_bytes, unsigned(r.fixed_delay_input_samples),
                r.frequency_dependent_delay ? "true" : "false", r.alias_warning ? "true" : "false");
    std::fflush(stdout);
}
std::size_t process_whole(PreparedResampler &op, std::span<const float> all_input, std::size_t packet,
                           std::span<float> output, std::span<float> whole) {
    std::size_t whole_count = 0;
    for (unsigned block = 0; block < 4u; ++block) {
        const auto block_input = all_input.subspan(block * packet, packet);
        std::size_t n = 0;
        check(op.process(block_input, packet, packet, output, output.size(), n), "whole processing");
        check(whole_count + n <= whole.size(), "bounded whole oracle");
        std::copy_n(output.begin(), n, whole.begin() + whole_count);
        whole_count += n;
        vTaskDelay(1);
    }
    return whole_count;
}
void process_fragments(PreparedResampler &op, std::span<const float> block_input, std::span<float> fragment,
                       std::span<const float> whole, std::size_t whole_count, std::size_t &total, double &maximum) {
    for (std::size_t at = 0, step = 0; at < block_input.size(); ++step) {
        const auto count = std::min(block_input.size() - at, 1 + (step * 37) % 97);
        std::size_t n = 0;
        check(op.process(block_input.subspan(at, count), count, count, fragment, fragment.size(), n),
              "fragmented processing");
        check(total + n <= whole_count, "fragment count bounded");
        for (std::size_t i = 0; i < n; ++i) {
            check(std::isfinite(fragment[i]), "finite output");
            if (total + i < whole_count)
                maximum = std::max(maximum, std::abs(double(fragment[i]) - whole[total + i]));
        }
        total += n;
        at += count;
    }
}
void verify_partitions(PreparedResampler &op, std::span<const float> all_input, std::size_t packet,
                        std::span<float> fragment, std::span<const float> whole, std::size_t whole_count,
                        std::size_t vendor_allocations, ResamplerMode mode, unsigned in, unsigned out) {
    std::size_t total = 0;
    double maximum = 0;
    for (unsigned block = 0; block < 4u; ++block) {
        const auto block_input = all_input.subspan(block * packet, packet);
        process_fragments(op, block_input, fragment, whole, whole_count, total, maximum);
        vTaskDelay(1);
    }
    check(total == whole_count && maximum < 2e-6, "packet partition invariance");
    check(qtvis_speex_allocation_calls() == vendor_allocations, "no vendor process allocation");
    std::printf("{\"check\":\"partition\",\"mode\":\"%s\",\"in\":%u,\"out\":%u,\"packet\":%zu,\"outputs\":%zu,"
                "\"max_abs\":%.9g}\n",
                name(mode), in, out, packet, total, maximum);
}
void time_mode(PreparedResampler &op, ResamplerMode mode, unsigned in, unsigned out, std::size_t packet,
                std::span<const float> input, std::span<float> output) {
    op.reset();
    const auto heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    measure(name(mode), in, out, packet, op.resources().owned_bytes, [&](std::size_t &n) {
        const auto ok = op.process(input, packet, packet, output, output.size(), n);
        if (n)
            sink = output[n - 1];
        return ok;
    });
    check(heap_before == heap_caps_get_free_size(MALLOC_CAP_8BIT), "stable processing heap");
}
void benchmark_mode(ResamplerMode mode, unsigned in, unsigned requested, std::size_t packet,
                     const std::vector<float> &input_storage, std::span<const float> all_input) {
    const auto out = mode == ResamplerMode::Bypass ? in : requested;
    const auto capacity = PreparedResampler::maximum_output(packet, in, out);
    const auto fragment_capacity = PreparedResampler::maximum_output(97, in, out);
    const dsp_kernels::ResamplerConfig config{mode, in, out, 1, 5};
    PreparedResampler op;
    dsp_kernels::ResamplerResources query;
    const auto prepare_us = prepare_mode(op, config, query);
    // Allocate the coefficient bank before output staging, preserving internal heap regions.
    ModeBuffers buffers(capacity, fragment_capacity);
    report_mode(op, query, mode, in, out, packet, prepare_us);
    const auto vendor_allocations = qtvis_speex_allocation_calls();
    const auto whole_count = process_whole(op, all_input, packet, buffers.output, buffers.whole);
    op.reset();
    verify_partitions(op, all_input, packet, buffers.fragment, buffers.whole, whole_count,
                      vendor_allocations, mode, in, out);
    time_mode(op, mode, in, out, packet, all_input.first(packet), buffers.output);
    check(input_storage.front() == guard && input_storage.back() == guard && buffers.output_storage.front() == guard &&
              buffers.output_storage.back() == guard,
          "mode canaries");
    check(heap_caps_check_integrity_all(true), "mode heap integrity");
}
void benchmark(unsigned in, unsigned requested, std::size_t packet) {
    // One continuous synthetic sequence, generated before any processing timer.
    std::vector<float> input_storage(4 * packet + 2, guard);
    auto all_input = std::span(input_storage).subspan(1, 4 * packet);
    fill(all_input, in, 0);
    for (const auto mode : {ResamplerMode::Bypass, ResamplerMode::ExistingFir, ResamplerMode::CompactFir,
                            ResamplerMode::SpeexDsp, ResamplerMode::IirExperimental, ResamplerMode::LinearDiagnostic}) {
        if (mode == ResamplerMode::Bypass && requested != 32768)
            continue;
        benchmark_mode(mode, in, requested, packet, input_storage, all_input);
    }
}
void run_benchmark_routes() {
    for (const auto in : {48000u, 44100u})
        for (const auto out : {32768u, 32000u}) {
            fir_oracle(in, out);
            for (const auto packet : {2048u, 4096u}) {
                benchmark(in, out, packet);
                vTaskDelay(1);
            }
        }
}
} // namespace
extern "C" void app_main() {
    const auto boot_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    (void)desktop_resampling::speex_backend_factory();
    const auto factory_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    std::printf("RESAMPLER_BENCH_BOOTSTRAP formatting_warmup=%.9g\n", 1.23456789);
    const auto before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const auto start = esp_timer_get_time();
    std::printf(
        "RESAMPLER_BENCH_BEGIN cpu_mhz=%lu free_heap=%zu factory_bootstrap_bytes=%zu formatting_bootstrap_bytes=%zu\n",
        (unsigned long)esp_rom_get_cpu_ticks_per_us(), before, boot_heap - factory_heap, factory_heap - before);
    finite_classification();
    check(dot_probe::run(), "dot probe accuracy");
    run_benchmark_routes();
    check(heap_caps_check_integrity_all(true), "final heap integrity");
    const auto after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    check(after == before, "whole-run heap restored after cold bootstrap");
    std::printf("RESAMPLER_BENCH_%s free_before=%zu free_after=%zu minimum_free=%zu elapsed_ms=%lld\n",
                success ? "PASS" : "FAIL", before, after, heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                (long long)((esp_timer_get_time() - start) / 1000));
    std::fflush(stdout);
    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
