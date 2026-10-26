#include "rfft_numerical_oracle.h"
#include "dsp_kernels/dsp.h"
#include "../libs/dsp_kernels/src/backend.h"
#include "../libs/dsp_kernels/src/rfft_split.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <fstream>

namespace {
std::atomic<bool> watch_allocations{false};
std::atomic<unsigned> allocations{0};
unsigned aligned_failure_countdown=0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double absolute, double relative, const char* message) {
    require(std::abs(actual - expected) <= absolute + relative * std::abs(expected), message);
}
using rfft_test_oracle::direct_dft;
using rfft_test_oracle::signal;
using rfft_test_oracle::expected_projection;
void verify_fused_reconstruction(std::span<const float> input,std::vector<float>& raw,std::span<const double> oracle) {
    using namespace dsp_kernels;
    const auto n=input.size();
    if(n>=16 && ((n/2)&0x55555555)==0) {
        const auto h=n/4;
        detail::FftStorage packed(n), twiddles(n/2+2);
        for(std::size_t i=0;i<h;++i) {
            packed[2*i]=input[4*i]; packed[2*i+1]=input[4*i+1];
            packed[2*h+2*i]=input[4*i+2]; packed[2*h+2*i+1]=input[4*i+3];
        }
        for(std::size_t k=0;k<=h;++k) {
            const auto angle=-2*std::numbers::pi*double(k)/double(n);
            twiddles[2*k]=.5f*float(std::cos(angle)); twiddles[2*k+1]=.5f*float(std::sin(angle));
        }
        twiddles[2*h]=0;twiddles[2*h+1]=-.5f;
        require(detail::prepare_backend(h,true) && detail::transform(packed.data(),h,true) &&
            detail::transform(packed.data()+2*h,h,true),"Fused kernel setup failed");
        allocations=0;watch_allocations=true;
        detail::rfft_mixed_split(raw.data(),packed.data(),twiddles.data(),h);
        watch_allocations=false;
        require(allocations==0,"Fused reconstruction allocates");
        for(std::size_t k=0;k<n+2;++k) near(raw[k],oracle[k],.0003,5e-6,"Fused reconstruction differs from DFT");
    }
}
void verify_full_fft_routes(std::span<const float> input,std::vector<float>& raw,std::span<const double> oracle) {
    using namespace dsp_kernels;
    const auto n=input.size();
    for (bool radix4 : {false, true}) {
        if (radix4 && (n & 0x55555555) == 0) continue;
        FFT full;
        require(full.prepare(n, radix4), "Raw full FFT prepare failed");
        allocations = 0;
        watch_allocations = true;
        const bool ok = full.process(input, raw);
        watch_allocations = false;
        require(ok && allocations == 0, "Raw full FFT fails or allocates");
        for (std::size_t i = 0; i < raw.size(); ++i)
            near(raw[i], oracle[i], .0003, 5e-6, "Raw full FFT differs from DFT");
    }
}
void verify_projections(std::span<const float> input,const std::vector<float>& raw,std::vector<float>& projected,
    const dsp_kernels::WindowScaling& scaling) {
    using namespace dsp_kernels;
    const auto n=input.size();
    const auto preserved = raw;
    for (const auto kind : {SpectrumProjection::Real, SpectrumProjection::Imaginary, SpectrumProjection::Phase,
            SpectrumProjection::Magnitude, SpectrumProjection::PeakAmplitude, SpectrumProjection::Power,
            SpectrumProjection::Psd, SpectrumProjection::RmsAmplitude, SpectrumProjection::BinPower}) {
        PreparedSpectrumProjection projector;
        require(projector.prepare(n, 32768, scaling, {kind,kind!=SpectrumProjection::Magnitude}), "Projection prepare failed");
        allocations = 0;
        watch_allocations = true;
        const bool projected_ok = projector.process(raw, projected);
        watch_allocations = false;
        require(projected_ok && allocations == 0 && raw == preserved, "Projection fails, allocates or mutates raw DFT");
        for (std::size_t k = 0; k < projected.size(); ++k) {
            const double expected = expected_projection(kind, raw[2 * k], raw[2 * k + 1],
                k == 0 || k == n / 2 ? 1 : 2, scaling, 32768);
            near(projected[k], expected, 1e-5, 2e-6, "Projection formula/reference incorrect");
        }
        if (kind == SpectrumProjection::BinPower) {
            double integrated=0,windowed_squares=0;
            for(const auto value:projected)integrated+=value;
            for(const double value:input)windowed_squares+=value*value;
            near(integrated,windowed_squares/scaling.sum_squares,1e-7,3e-6,"Bin power Parseval identity failed");
        }
        if (kind == SpectrumProjection::Psd) {
            double integrated = 0, windowed_squares = 0;
            for (const auto value : projected) integrated += value * (32768. / double(n));
            for (const double value : input) windowed_squares += value * value;
            near(integrated, windowed_squares / scaling.sum_squares, 1e-7, 3e-6, "PSD Parseval identity failed");
        }
    }
}
struct NumericalRoute {
    std::size_t n;
    std::span<const float> input;
    std::span<const double> oracle;
    const dsp_kernels::WindowScaling& scaling;
    std::vector<float>& raw;
    std::vector<float>& radix2;
    std::vector<float>& projected;
    double maximum_error=0;
    void process(bool radix4,dsp_kernels::PreparedRfft::Split split,bool platform) {
        using namespace dsp_kernels;
        PreparedRfft rfft;
        const PreparedRfft::Options options{platform};
        require(rfft.prepare(n, radix4, split, &options), "RFFT prepare failed");
        require(rfft.samples() == n && rfft.bins() == n / 2 + 1, "RFFT geometry incorrect");
        const auto workspace = rfft.workspace_bytes();
        allocations = 0;
        watch_allocations = true;
        const bool processed = rfft.process(input, raw);
        watch_allocations = false;
        require(processed && allocations == 0, "Prepared RFFT fails or allocates");
        require(rfft.workspace_bytes() == workspace, "RFFT scratch grew");
        require(raw[1] == 0 && raw[n + 1] == 0, "DC/Nyquist have nonzero imaginary part");
        for (std::size_t i = 0; i < raw.size(); ++i) {
            maximum_error = std::max(maximum_error, std::abs(raw[i] - oracle[i]));
            near(raw[i], oracle[i], .0003, 5e-6, "RFFT Re/Im differs from DFT");
            if (radix4) near(raw[i], radix2[i], .0003, 5e-6, "Radix-4 differs from radix-2");
        }
        if (!radix4) radix2 = raw;
        verify_projections(input,raw,projected,scaling);
    std::cout << "resource N=" << n << " radix=" << (radix4 ? 4 : 2) << " split=" << rfft.split_implementation()
        << " workspace_bytes=" << workspace << '\n';
    }
};
void numerical_case(std::size_t n, dsp_kernels::WindowFunction window) {
    using namespace dsp_kernels;
    auto input = signal(n);
    std::vector<float> coefficients(n);
    dsp_kernels::window(coefficients, window);
    const auto scaling = window_scaling(coefficients);
    require(scaling.has_value(), "Finite window rejected");
    double sum = 0, squares = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double angle = 2 * std::numbers::pi * double(i) / double(n - 1);
        const double exact = window == WindowFunction::Rectangular ? 1 : window == WindowFunction::Hann ? .5 - .5 * std::cos(angle) :
            window == WindowFunction::Hamming ? .54 - .46 * std::cos(angle) : .42 - .5 * std::cos(angle) + .08 * std::cos(2 * angle);
        sum += exact;
        squares += exact * exact;
        input[i] *= coefficients[i];
    }
    near(scaling->sum, sum, .0005, 1e-6, "Window sum differs from symmetric formula");
    near(scaling->sum_squares, squares, .0005, 1e-6, "Window squares differ from symmetric formula");
    near(scaling->coherent_gain(), sum / double(n), 1e-7, 1e-6, "Coherent gain wrong");
    const auto oracle = direct_dft(input);

    std::vector<float> raw(n+2),radix2(n+2),projected(n/2+1);
    verify_fused_reconstruction(input,raw,oracle);
    verify_full_fft_routes(input,raw,oracle);
    NumericalRoute route{n,input,oracle,*scaling,raw,radix2,projected};
    for(bool radix4:{false,true})
        for(auto split:{PreparedRfft::Split::Scalar,PreparedRfft::Split::Automatic})
            for(bool platform:{false,true})route.process(radix4,split,platform);
    std::cout << "oracle N=" << n << " window=" << unsigned(window) << " max_abs_error=" << route.maximum_error << '\n';
}
void endpoint_calibrations(dsp_kernels::PreparedRfft& fft,dsp_kernels::PreparedSpectrumProjection& projection,
    std::span<float> input,std::span<float> raw,std::span<float> output,const dsp_kernels::WindowScaling& scaling) {
    using namespace dsp_kernels;const auto n=input.size();
    for (std::size_t k : {std::size_t{0}, std::size_t{17}, n / 2}) {
        for (std::size_t i = 0; i < n; ++i) input[i] = float(.7 * std::cos(2 * std::numbers::pi * double(k * i) / double(n)));
        require(fft.process(input, raw) && projection.prepare(n, 32768, scaling, {SpectrumProjection::PeakAmplitude,true}) && projection.process(raw, output), "Tone peak failed");
        near(output[k], .7, 1e-6, 3e-6, "One-sided peak endpoint factor wrong");
        require(projection.prepare(n, 32768, scaling, {SpectrumProjection::Power,true}) && projection.process(raw, output), "Tone power failed");
        near(output[k], k == 17 ? .245 : .49, 1e-6, 3e-6, "Power uses squared peak instead of mean-square");
        require(projection.prepare(n,32768,scaling,{SpectrumProjection::RmsAmplitude,true}) && projection.process(raw,output),"RMS amplitude failed");
        near(output[k],k==17?.7/std::sqrt(2.):.7,1e-6,3e-6,"RMS amplitude factor differs from power");
        require(projection.prepare(n,32768,scaling,{SpectrumProjection::BinPower,true}) && projection.process(raw,output),"Bin power failed");
        near(output[k],(k==17?.245:.49)*scaling.sum*scaling.sum/(double(n)*scaling.sum_squares),1e-6,3e-6,"Bin power uses wrong window normalization");
    }
}
void edge_cases(std::size_t n) {
    using namespace dsp_kernels;
    PreparedRfft fft;
    PreparedSpectrumProjection projection;
    std::vector<float> input(n), raw(n + 2), output(n / 2 + 1), window(n, 1);
    const auto scaling = *window_scaling(window);
    require(!fft.prepare(7) && !fft.prepare(768) && !fft.prepare(max_fft_samples * 2), "Invalid RFFT prepare accepted");
    require(fft.prepare(n), "Valid RFFT prepare rejected");
    for (const auto kind : {SpectrumProjection::Magnitude, SpectrumProjection::PeakAmplitude, SpectrumProjection::Power, SpectrumProjection::Psd, SpectrumProjection::RmsAmplitude, SpectrumProjection::BinPower}) {
        require(projection.prepare(n, 32768, scaling, {kind,kind!=SpectrumProjection::Magnitude}) && fft.process(input, raw) && projection.process(raw, output), "Silence processing failed");
        require(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }), "Linear silence is not zero");
    }
    endpoint_calibrations(fft,projection,input,raw,output,scaling);
    input[0] = std::numeric_limits<float>::infinity();
    const auto old_raw = raw;
    require(!fft.process(input, raw) && raw == old_raw, "Nonfinite RFFT is not atomic");
    input[0] = std::numeric_limits<float>::quiet_NaN();
    require(!fft.process(input, raw) && raw == old_raw, "NaN RFFT accepted");
    std::fill(input.begin(), input.end(), std::numeric_limits<float>::max());
    require(!fft.process(input, raw) && raw == old_raw, "Raw overflow was not atomic");
    const auto old_output = output;
    raw.back() = std::numeric_limits<float>::quiet_NaN();
    require(!projection.process(raw, output) && output == old_output, "Nonfinite projection is not atomic");
    raw = old_raw;
    require(!projection.process(raw, std::span(raw).first(output.size())) && raw == old_raw, "Projection alias changed raw DFT");
    require(!projection.prepare(n, 0, scaling, {SpectrumProjection::Psd,true}) &&
        !projection.prepare(n, 32768, {n, 0, 0}, {SpectrumProjection::Psd,true}) &&
        !projection.prepare(n, 32768, scaling, {SpectrumProjection(999),true}), "Invalid projection accepted");
    PreparedSpectrumScaling extreme;
    require(!extreme.prepare(1e308,1,1e-320,{SpectrumProjection::BinPower,true},n),"overflowed bin-power calibration is rejected at prepare");
    require(!extreme.prepare(48000,1,1,{SpectrumProjection::BinPower,true}),"bin power requires an explicit FFT length");
    window[0] = std::numeric_limits<float>::infinity();
    require(!window_scaling(window) && !window_scaling({}), "Invalid window statistics accepted");
    // Scaling avoids intermediate overflow even when the raw result is finite.
    std::fill(input.begin(), input.end(), 0);
    input[0] = std::numeric_limits<float>::max() / 2;
    require(fft.process(input, raw), "Representable large impulse failed");
    near(raw[0], input[0], 0, 2e-6, "Large impulse scale lost");
    for (bool radix4 : {false, true}) {
        require(fft.prepare(n, radix4) && fft.process(input, raw), "Scaled radix route failed");
        for (std::size_t k = 0; k <= n / 2; ++k) {
            near(raw[2 * k], input[0], 0, 2e-6, "Scaled impulse bin wrong");
            require(raw[2 * k + 1] == 0, "Scaled impulse imaginary bin wrong");
        }
        input[0] = std::numeric_limits<float>::denorm_min();
        require(fft.process(input, raw), "Subnormal impulse failed");
        require(raw[0] == input[0] && raw[n] == input[0], "Normal-path copy lost subnormal endpoints");
        input[0] = std::numeric_limits<float>::max() / 2;
    }
}
constexpr std::array projection_kinds{
    dsp_kernels::SpectrumProjection::Real, dsp_kernels::SpectrumProjection::Imaginary,
    dsp_kernels::SpectrumProjection::Phase, dsp_kernels::SpectrumProjection::Magnitude,
    dsp_kernels::SpectrumProjection::PeakAmplitude, dsp_kernels::SpectrumProjection::Power,
    dsp_kernels::SpectrumProjection::Psd, dsp_kernels::SpectrumProjection::RmsAmplitude,
    dsp_kernels::SpectrumProjection::BinPower,dsp_kernels::SpectrumProjection::WindowScaledMagnitude};
constexpr std::array projection_names{"real", "imaginary", "phase", "magnitude", "peak_amplitude", "power", "psd", "rms_amplitude", "bin_power","window_scaled_magnitude"};
void fill_extreme_projection(std::size_t n,unsigned frame,std::vector<float>& raw) {
        for (std::size_t k = 0; k <= n / 2; ++k) {
            const float magnitude = frame == 0 ? (k % 2 ? std::numeric_limits<float>::denorm_min() : std::numeric_limits<float>::min()) :
                frame == 1 ? (k % 2 ? std::numeric_limits<float>::max() : std::numeric_limits<float>::max() / 4) :
                (k % 2 ? 1e-20f : 1e20f);
            raw[2 * k] = magnitude;
            raw[2 * k + 1] = k == 0 || k == n / 2 ? 0 : -magnitude;
        }
}
void verify_extreme_projection(std::size_t n,dsp_kernels::SpectrumProjection kind,std::vector<float>& raw,
    std::vector<float>& output,std::vector<float>& expected,const std::vector<float>& original,
    const dsp_kernels::WindowScaling& scaling) {
    using namespace dsp_kernels;
    constexpr double rate=32768;
            PreparedSpectrumProjection projection;
            require(projection.prepare(n, rate, scaling, {kind,kind!=SpectrumProjection::Magnitude}), "Extreme projection prepare failed");
            bool representable = true;
            for (std::size_t k = 0; k <= n / 2; ++k) {
                expected[k] = float(expected_projection(kind, raw[2 * k], raw[2 * k + 1], k == 0 || k == n / 2 ? 1 : 2,
                    scaling, rate));
                representable = representable && std::isfinite(expected[k]);
            }
            std::fill(output.begin(), output.end(), -321.f);
            allocations = 0;
            watch_allocations = true;
            const bool processed = projection.process(raw, output);
            watch_allocations = false;
            require(processed == representable && allocations == 0 && raw == original, "Extreme projection status/allocation/immutability failure");
            if (representable) for (std::size_t k = 0; k <= n / 2; ++k)
                near(output[k], expected[k], 1e-40, 3e-6, "Extreme projection differs from hypot oracle");
            else require(std::all_of(output.begin(), output.end(), [](float value) { return value == -321.f; }), "Extreme projection overflow is not atomic");
            const auto preserved = output;
            raw.back() = std::numeric_limits<float>::quiet_NaN();
            require(!projection.process(raw, output) && output == preserved, "Projection late NaN is not atomic");
            raw.back() = original.back();
}
void projection_extremes(std::size_t n) {
    std::vector<float> raw(n+2),output(n/2+1),expected(n/2+1);
    const dsp_kernels::WindowScaling scaling{n,double(n),double(n)};
    for(unsigned frame=0;frame<3;++frame) {
        fill_extreme_projection(n,frame,raw);
        const auto original=raw;
        for(const auto kind:projection_kinds)
            verify_extreme_projection(n,kind,raw,output,expected,original,scaling);
    }
}
void projection_benchmark(std::size_t n) {
    using namespace dsp_kernels;
    constexpr std::size_t repetitions = 600;
    const auto input = signal(n);
    std::vector<float> raw(n + 2), output(n / 2 + 1);
    PreparedRfft fft;
    require(fft.prepare(n) && fft.process(input, raw), "Projection benchmark RFFT failed");
    const WindowScaling scaling{n, double(n), double(n)};
    for (std::size_t index = 0; index < projection_kinds.size(); ++index) {
        PreparedSpectrumProjection projection;
        require(projection.prepare(n, 32768, scaling, {projection_kinds[index],projection_kinds[index]!=SpectrumProjection::Magnitude}), "Projection benchmark prepare failed");
        std::array<double, repetitions> elapsed{};
        for (unsigned warm = 0; warm < 24; ++warm) require(projection.process(raw, output), "Projection benchmark warmup failed");
        allocations = 0;
        watch_allocations = true;
        for (auto& value : elapsed) {
            const auto begin = std::chrono::steady_clock::now();
            require(projection.process(raw, output), "Projection benchmark process failed");
            value = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
        }
        watch_allocations = false;
        require(allocations == 0, "Projection benchmark allocates");
        std::sort(elapsed.begin(), elapsed.end());
        std::cout << "projection N=" << n << " kind=" << projection_names[index]
            << " p50_us=" << elapsed[repetitions / 2] << " p95_us=" << elapsed[repetitions * 95 / 100]
            << " workspace_bytes=" << projection.workspace_bytes() << " allocations=" << allocations << '\n';
    }
}
void raw_contract_cases() {
    using namespace dsp_kernels;
    constexpr auto n = max_fft_samples;
    detail::FftStorage aligned(2*n);
    require(reinterpret_cast<std::uintptr_t>(aligned.data()) % 16 == 0, "FFT storage is not SIMD aligned");
    std::vector<float> input(n), output(n + 2, 19.f);
    const auto exercise = [&](auto& fft) {
        const auto preserved = output;
        input.back() = std::numeric_limits<float>::quiet_NaN();
        require(!fft.process(input, output) && output == preserved, "Raw late NaN is not atomic");
        input.back() = std::numeric_limits<float>::infinity();
        require(!fft.process(input, output) && output == preserved, "Raw infinity accepted");
        std::fill(input.begin(), input.end(), std::numeric_limits<float>::max());
        require(!fft.process(input, output) && output == preserved, "Unrepresentable raw DC accepted");
        std::fill(input.begin(), input.end(), 0);
        input[0] = std::numeric_limits<float>::max() / 2;
        require(fft.process(input, output), "Representable raw impulse rejected");
        for (std::size_t k = 0; k <= n/2; ++k) {
            near(output[2*k], input[0], 0, 3e-6, "Raw impulse scaling incorrect");
            require(output[2*k+1] == 0, "Raw impulse imaginary incorrect");
        }
        input[0] = std::numeric_limits<float>::denorm_min();
        require(fft.process(input, output) && output[0] == input[0] && output[n] == input[0], "Raw subnormal lost");
        std::fill(input.begin(), input.end(), 0);
        for (std::size_t i=0;i<n;++i) input[i] = i%2 ? -1.f : 1.f;
        require(fft.process(input, output) && output[n] == float(n) && output[n+1] == 0, "Raw Nyquist lost");
        fft.reset();
        require(fft.samples() == 0 && fft.bins() == 0 && fft.workspace_bytes() == 0 && !fft.process(input, output), "Raw reset retains plan");
    };
    for (bool radix4 : {false,true}) {
        if (!radix4 || (n & 0x55555555)) {
            FFT full; require(full.prepare(n,radix4), "Raw full prepare failed");
            exercise(full);
            require(full.prepare(n,radix4) && !full.prepare(7) && !full.process(input,output), "Raw failed prepare leaves live full plan");
        }
        for (auto split : {PreparedRfft::Split::Scalar,PreparedRfft::Split::Automatic}) {
            PreparedRfft real; require(real.prepare(n,radix4,split), "Raw real prepare failed");
            exercise(real);
            require(real.prepare(n,radix4,split) && !real.prepare(n,radix4,PreparedRfft::Split(999)) &&
                !real.process(input,output), "Raw invalid split leaves live plan");
        }
    }
}

void allocation_failure_cases() {
    using namespace dsp_kernels;
    constexpr auto n=(max_fft_samples&0x55555555u)?max_fft_samples:max_fft_samples/2;
    std::vector<float> input(n),output(n+2,19.f);
    FFT full;
    aligned_failure_countdown=1;
    require(!full.prepare(n,true) && full.samples()==0,"Raw FFT allocation failure accepted");
    require(!full.process(input,output) && output.front()==19.f,"Failed raw allocation published output");
    aligned_failure_countdown=0;
    for(unsigned failure:{1u,2u,3u}) {
        PreparedRfft plan;
        aligned_failure_countdown=failure;
        require(!plan.prepare(n,true) && plan.samples()==0,"RFFT allocation failure accepted");
        aligned_failure_countdown=0;
        require(!plan.process(input,output) && std::all_of(output.begin(),output.end(),[](float x){return x==19.f;}),"Allocation failure was not atomic");
        plan.reset();
        require(plan.workspace_bytes()==0,"Reset leaked partially allocated buffers");
        require(plan.prepare(n,true) && plan.process(input,output),"Retry after instance allocation failure failed");
        std::fill(output.begin(),output.end(),19.f);
    }
}
struct RawBenchmark {
    static constexpr std::size_t n=4096,routes=6,frames=8,batches=240,batch=32;
    std::array<dsp_kernels::FFT,2> full;
    std::array<dsp_kernels::PreparedRfft,4> real;
    std::array<std::vector<float>,frames> input,oracle;
    std::array<std::vector<float>,routes> output;
    std::array<std::array<double,batches>,routes> elapsed{};
    double max_error=0;
    void prepare() {
        using namespace dsp_kernels;
    std::vector<float> coefficients(n);
    window(coefficients, WindowFunction::Hann);
    for (std::size_t f = 0; f < frames; ++f) {
        input[f] = signal(n);
        std::rotate(input[f].begin(), input[f].begin() + f * 13, input[f].end());
        for (std::size_t i = 0; i < n; ++i) input[f][i] *= coefficients[i];
        oracle[f].resize(n + 2);
    }
    for (auto& values : output) values.resize(n + 2);
    require(full[0].prepare(n, false) && full[1].prepare(n, true), "Raw CFFT prepare failed");
    for (std::size_t i = 0; i < real.size(); ++i)
        require(real[i].prepare(n, i >= 2, i % 2 ? PreparedRfft::Split::Automatic : PreparedRfft::Split::Scalar), "Raw RFFT prepare failed");
    for (std::size_t f = 0; f < frames; ++f) require(full[0].process(input[f], oracle[f]), "Raw oracle failed");
    }
    void call(std::size_t route,std::size_t frame) {
        const bool ok = route < 2 ? full[route].process(input[frame], output[route])
            : real[route - 2].process(input[frame], output[route]);
        require(ok, "Raw benchmark transform failed");
    }
    void verify_and_warm() {
    for (std::size_t route = 0; route < routes; ++route) {
        for (std::size_t f = 0; f < frames; ++f) {
            call(route, f);
            for (std::size_t k = 0; k < n + 2; ++k) {
                max_error = std::max(max_error, std::abs(double(output[route][k]) - oracle[f][k]));
                near(output[route][k], oracle[f][k], .0003, 5e-6, "Raw routes disagree");
            }
        }
        for (std::size_t warm = 0; warm < 64; ++warm) call(route, warm % frames);
    }
    }
    void measure() {
    allocations = 0;
    watch_allocations = true;
    for (std::size_t b = 0; b < batches; ++b) for (std::size_t order = 0; order < routes; ++order) {
        const auto route = (b + order) % routes;
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < batch; ++i) call(route, (b * batch + i) % frames);
        elapsed[route][b] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / batch;
    }
    watch_allocations = false;
    require(allocations == 0, "Raw benchmark allocates");
    }
    void report() {
    constexpr std::array names{"CFFT-radix2", "CFFT-radix4", "RFFT-radix2-scalar", "RFFT-radix2-auto", "RFFT-radix4-scalar", "RFFT-radix4-auto"};
    for (std::size_t route = 0; route < routes; ++route) {
        auto& values = elapsed[route];
        std::sort(values.begin(), values.end());
        std::cout << "raw_benchmark N=4096 route=" << names[route]
            << " split=" << (route < 2 ? "none" : real[route-2].split_implementation())
            << " p50_us=" << values[batches/2] << " p95_batch_mean_us=" << values[batches*95/100]
            << " workspace_bytes=" << (route < 2 ? full[route].workspace_bytes() : real[route-2].workspace_bytes())
            << " allocations=" << allocations << " transforms=" << batches*batch << '\n';
    }
    std::cout << "raw_route_max_abs_difference=" << max_error << '\n';    }
};
void raw_benchmark() {
    if(RawBenchmark::n>dsp_kernels::max_fft_samples)return;
    RawBenchmark benchmark;
    benchmark.prepare();
    benchmark.verify_and_warm();
    benchmark.measure();
    benchmark.report();
}

void json_values(std::ostream& out,std::span<const float> values) {
    out<<'[';
    for(std::size_t i=0;i<values.size();++i){if(i)out<<',';out<<values[i];}
    out<<']';
}
float oracle_sample(unsigned kind,std::size_t i,std::size_t fft_size,std::uint32_t& random) {
    const auto angle=2*std::numbers::pi*double(i)/double(fft_size);
    switch(kind){
    case 0:return 0;
    case 1:return .25f;
    case 2:return i%2?-.3f:.3f;
    case 3:return i==7?.7f:0;
    case 4:return float(.5*std::cos(7*angle+.2));
    case 5:return float(.5*std::cos(7.37*angle+.2));
    case 6:random=random*1664525u+1013904223u;return float(double(random)/double(UINT32_MAX)-.5);
    default:{double value=.1;for(unsigned tone=0;tone<7;++tone)value+=.2/(tone+1)*std::cos((3+4.25*tone)*angle+.17*tone);return float(value);}
    }
}
struct ProjectionExport {
    std::vector<float> signal,window,windowed,raw,full,values;
    dsp_kernels::WindowScaling scaling;
    std::size_t samples;
    explicit ProjectionExport(std::size_t size,dsp_kernels::WindowFunction function,unsigned kind,std::size_t padded)
        :signal(size),window(size),windowed(padded),raw(padded+2),full(padded+2),values(padded/2+1),samples(padded) {
        std::uint32_t random=0x56341527u;
        const dsp_kernels::PreparedWindows bank(size);const auto coefficients_view=bank.coefficients(function);
        for(std::size_t i=0;i<size;++i){
            window[i]=coefficients_view[i];near(window[i],dsp_kernels::window_coefficient(i,size,function),1e-7,0,"prepared and scalar windows disagree");
            signal[i]=oracle_sample(kind,i,padded,random);windowed[i]=signal[i]*window[i];
        }
        auto coefficients=window;coefficients.resize(padded,0);scaling=*dsp_kernels::window_scaling(coefficients);
        dsp_kernels::PreparedRfft rfft;dsp_kernels::FFT fft;
        require(rfft.prepare(padded) && fft.prepare(padded),"oracle FFT plans");
        allocations=0;watch_allocations=true;
        const bool success=rfft.process(windowed,raw) && fft.process(windowed,full);watch_allocations=false;
        require(success && allocations==0,"export FFT routes have no hot allocations");
        for(std::size_t i=0;i<raw.size();++i)near(raw[i],full[i],.0003,5e-6,"CFFT and RFFT oracle routes disagree");
    }
    void outputs(std::ostream& out,double rate) {
        bool comma=false;
        for(const auto kind:projection_kinds)for(const auto compensate:{false,true}){
            dsp_kernels::PreparedSpectrumProjection plan;
            require(plan.prepare(samples,rate,scaling,{kind,compensate}),"oracle projection plan");
            allocations=0;watch_allocations=true;const bool success=plan.process(raw,values);watch_allocations=false;
            require(success && allocations==0,"export projections have no hot allocations");
            if(comma)out<<',';comma=true;
            out<<'"'<<dsp_kernels::spectrum_projection_name(kind)<<(compensate?"_scaled":"_raw")<<"\":";
            json_values(out,values);
        }
    }
    void write(std::ostream& out,dsp_kernels::WindowFunction function,unsigned kind,double rate) {
        out<<"{\"window_name\":\""<<dsp_kernels::window_function_name(function)<<"\",\"signal_kind\":"<<kind<<",\"fs\":"<<rate<<",\"nfft\":"<<samples;
        out<<",\"signal\":";json_values(out,signal);out<<",\"window\":";json_values(out,window);
        out<<",\"windowed\":";json_values(out,windowed);out<<",\"raw\":";json_values(out,raw);
        out<<",\"outputs\":{";outputs(out,rate);out<<"}}";
    }
};
void export_projections(const char* path) {
    std::ofstream out(path);out<<std::setprecision(std::numeric_limits<float>::max_digits10)<<'[';bool comma=false;
    for(const auto function:{dsp_kernels::WindowFunction::Hann,dsp_kernels::WindowFunction::Hamming,dsp_kernels::WindowFunction::Blackman,
            dsp_kernels::WindowFunction::Rectangular,dsp_kernels::WindowFunction::Bartlett,dsp_kernels::WindowFunction::BlackmanHarris,dsp_kernels::WindowFunction::FlatTop})
        for(const auto window_size:{63u,64u})for(const auto nfft:{64u,256u})for(unsigned kind=0;kind<8;++kind)for(const auto rate:{32768.,44100.,48000.}){
            if(comma)out<<',';comma=true;ProjectionExport fixture(window_size,function,kind,nfft);fixture.write(out,function,kind,rate);
        }
    out<<']';require(bool(out),"could not write spectral oracle export");
}

}
void* operator new(std::size_t bytes) {
    if (watch_allocations.load()) ++allocations;
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if(aligned_failure_countdown && --aligned_failure_countdown==0)throw std::bad_alloc();
    if (watch_allocations.load()) ++allocations;
    const auto align = std::size_t(alignment);
#if defined(_WIN32)
    auto* memory = _aligned_malloc(bytes ? bytes : align, align);
#else
    auto* memory = std::aligned_alloc(align, ((bytes ? bytes : 1) + align - 1) / align * align);
#endif
    if (!memory) throw std::bad_alloc();
    return memory;
}
void* operator new(std::size_t bytes,std::align_val_t alignment,const std::nothrow_t&) noexcept {
    try{return ::operator new(bytes,alignment);}catch(...){return nullptr;}
}
void operator delete(void* memory, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept { ::operator delete(memory, alignment); }
int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(9);
        if(argc==3 && std::string_view(argv[1])=="--export-projections"){export_projections(argv[2]);return 0;}
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark-only") {
            raw_benchmark();
            return 0;
        }
        raw_contract_cases();
        allocation_failure_cases();
        for (const auto n : {std::size_t{8}, std::size_t{16}, std::size_t{32}, std::size_t{512}, std::size_t{1024}, std::size_t{2048}, std::size_t{4096}}) {
            if (n > dsp_kernels::max_fft_samples) continue;
            for (const auto window : {dsp_kernels::WindowFunction::Hann, dsp_kernels::WindowFunction::Hamming,
                    dsp_kernels::WindowFunction::Blackman, dsp_kernels::WindowFunction::Rectangular}) numerical_case(n, window);

        }
        edge_cases(512);
        projection_extremes(512);

        if (dsp_kernels::max_fft_samples >= 4096) {
            edge_cases(4096);
            projection_extremes(4096);
            raw_benchmark();
        }
        std::cout << "Prepared RFFT numerical/resource regressions passed\n";
    } catch (const std::exception& error) {
        watch_allocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
