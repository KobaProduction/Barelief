#include "dsp_core/dsp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <span>

static std::atomic<bool> watch_allocations{false};
static std::atomic<unsigned> allocations{0};
void *operator new(std::size_t bytes) {
    if (watch_allocations.load())
        ++allocations;
    if (void *memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept {
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void near(float actual, float expected, const char *message) {
    require(std::abs(actual - expected) < 2e-5f * std::max(1.0f, std::abs(expected)), message);
}
static float dft_magnitude(std::span<const float> input,std::size_t bin){
    double re=0,im=0;
    for(std::size_t i=0;i<input.size();++i){const double angle=-2*std::numbers::pi*bin*i/input.size();re+=input[i]*std::cos(angle);im+=input[i]*std::sin(angle);}
    return float(std::hypot(re,im)/input.size());
}
static void verify_spectrum(std::span<const float> input,std::span<const float> output){
    for(std::size_t bin=0;bin<output.size();++bin){
        const auto fundamental=input.size()==4?std::hypot(.5f,.155f):.5f;
        const auto analytic=bin==0?.2f:bin==1?fundamental:bin==3?.155f:0.f;
        near(output[bin],analytic,"every FFT bin matches independent stationary-tone spectrum");
        if(input.size()<=64||bin==0||bin==1||bin==3||bin==input.size()/4||bin==output.size()-1)
            near(output[bin],dft_magnitude(input,bin),"FFT differs from independent direct DFT oracle");
    }
}
static void window_oracles(){
        for(const auto kind:{dsp_core::WindowFunction::Hann,dsp_core::WindowFunction::Hamming,dsp_core::WindowFunction::Blackman,dsp_core::WindowFunction::Rectangular}) {
            std::vector<float> coefficients(513);dsp_core::window(coefficients,kind);
            for(size_t i=0;i<coefficients.size();++i) near(coefficients[i],coefficients[coefficients.size()-1-i],"Window is not symmetric");
            near(coefficients[256],1,"Window center coefficient");
            near(coefficients.front(),kind==dsp_core::WindowFunction::Hamming?0.08f:kind==dsp_core::WindowFunction::Rectangular?1.f:0.f,"Window endpoint coefficient");
            const float angle=float(64*2*std::numbers::pi/512);
            near(coefficients[64],kind==dsp_core::WindowFunction::Hann?0.5f*(1-std::cos(angle)):
                kind==dsp_core::WindowFunction::Hamming?0.54f-0.46f*std::cos(angle):
                kind==dsp_core::WindowFunction::Blackman?0.42f-0.5f*std::cos(angle)+0.08f*std::cos(2*angle):1.f,"Window formula coefficient");
            std::vector<float> single(1);dsp_core::window(single,kind);near(single[0],1,"Single-sample window");
        }
        std::vector<float> legacy_hann(512),selected_hann(512);dsp_core::hann_window(legacy_hann);dsp_core::window(selected_hann,dsp_core::WindowFunction::Hann);
        require(legacy_hann==selected_hann,"Hann baseline changed");
}
static void memory_oracles(dsp_core::MagnitudeSpectrum& fft){
        require(!dsp_core::estimate_memory(511, 50) && !dsp_core::estimate_memory(768, 50) &&
                    !dsp_core::estimate_memory(dsp_core::max_fft_samples*2, 50) && !dsp_core::estimate_memory(4096, 51) &&
                    !dsp_core::estimate_memory(4096, 50, 17) && !dsp_core::estimate_memory(4096, 50, 16, 2001),
                "Invalid memory budget accepted");
        constexpr auto maximum = dsp_core::max_fft_samples;
        require(!dsp_core::estimate_memory(maximum * 2, 50), "Above-profile memory budget accepted");
        const auto memory = dsp_core::estimate_memory(maximum, 50, 16, 2000);
        require(memory && memory->queue_payload_bytes == 16 * 2 * maximum * sizeof(float) && memory->history_payload_bytes == 400000 &&
                    memory->fft_scratch_bytes == 2 * maximum * sizeof(float) && memory->mel_boundaries_bytes == 52 * sizeof(double),
                "Memory estimate has incorrect buffer geometry");
        // Exact payloads for every supported build profile (vendor allocations).
        const auto radix2_bytes = maximum>4096 ? maximum*sizeof(float) : maximum == 512 ? 3008 : maximum == 1024 ? 6080 : maximum == 2048 ? 12160 : 24448;
        const auto radix4_bytes = dsp_core::max_radix4_samples>4096 ? dsp_core::max_radix4_samples*4*sizeof(float) : maximum == 512 ? 4576 : maximum == 1024 || maximum == 2048 ? 18304 : 73600;
        require(memory->shared_radix2_tables_bytes == radix2_bytes && memory->shared_radix4_tables_bytes == radix4_bytes,
                "Shared FFT table estimate differs from vendor allocations");
        require(!fft.prepare(63) && !fft.prepare(maximum * 2) && !fft.prepare(maximum * 2, true),
                "Invalid or above-profile FFT size accepted");
}
static void fft_oracles(dsp_core::MagnitudeSpectrum& fft,dsp_core::MagnitudeSpectrum& other,std::size_t maximum){
        // Both explicitly magnitude-valued routes retain an independent DFT oracle.
        for (bool radix4 : {false, true})
        for (std::size_t n : {std::size_t{4}, std::size_t{16}, std::size_t{64}, maximum,
                             dsp_core::max_radix4_samples}) {
            if (radix4 && (n & 0x55555555) == 0) continue;
            require(fft.prepare(n, radix4), "FFT preparation failed");
            const auto workspace = fft.workspace_bytes();
            require(workspace >= 2 * n * sizeof(float), "FFT workspace excludes its scratch capacity");
            std::vector<float> input(n), output(n / 2);
            for (std::size_t i = 0; i < n; ++i)
                input[i] =
                    0.2f + std::sin(2 * std::numbers::pi * i / n) + 0.31f * std::cos(6 * std::numbers::pi * i / n);
            {
                require(fft.process(input, output), "Spectrum processing failed");
                verify_spectrum(input,output);
            }
            require(other.prepare(16, radix4), "Independent FFT failed");
            std::vector<float> dc(16, 1), dc_output(8);
            require(other.process(dc, dc_output), "Independent processing failed");
            near(dc_output[0], 1, "DC normalization wrong");
            require(fft.process(input, output), "Other instance invalidated prepared FFT");
            require(fft.workspace_bytes() == workspace, "FFT workspace changes during processing");
        }
}
static void mel_oracles(){
        dsp_core::MelFilterBank mel;
        std::vector<float> spectrum(32, 1), output(50, -1);
        mel.prepare(32, 0, 50);
        require(mel.process(spectrum, output), "Zero-rate Mel failed");
        require(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }), "Zero-rate Mel not silent");
        mel.prepare(32, 48000, 50);
        require(mel.process(spectrum, output), "Mel failed");
        require(mel.workspace_bytes() >= 52 * sizeof(double), "Mel workspace excludes boundary capacity");
        // Continuous-Hertz triangle oracle, independent of integer-bin rounding.
        std::vector<double> boundary(52);
        for(int i=0;i<52;++i)boundary[i]=700*(std::pow(1+24000./700,double(i)/51)-1);
        for(int m=1;m<=50;++m){double expected=0;
            for(int k=0;k<32;++k){const double hz=k*750.;
                expected+=std::max(0.,std::min((hz-boundary[m-1])/(boundary[m]-boundary[m-1]),
                    (boundary[m+1]-hz)/(boundary[m+1]-boundary[m])));}
            near(output[m-1],float(expected),"Continuous Hz Mel weights");
        }
}
static void stereo_oracles(dsp_core::MagnitudeSpectrum& fft){
        dsp_core::StereoProcessor processor;
        require(processor.prepare(64, 48000), "Stereo preparation failed");
        const auto stereo_workspace = processor.workspace_bytes();
        std::vector<float> left(64, 1), right(64, 0), window(64, 1), weights(32, 2), sl(32), sr(32), ml(50), mr(50);
        watch_allocations = true;
        const bool processed = processor.process(left, right, window, weights, sl, sr, ml, mr);
        watch_allocations = false;
        require(processed && allocations == 0, "Prepared processing allocates");
        require(processor.workspace_bytes() == stereo_workspace, "Stereo workspace grows during processing");
        near(sl[0], 2, "Bark weighting or normalization changed");
        require(std::all_of(sr.begin(), sr.end(), [](float x) { return x == 0; }), "Stereo channels leak");
        require(processor.process(left, right, window, weights, sl, sr), "Optional Mel suppression failed");
        require(!fft.process(std::span<const float>{}, sl), "Wrong FFT input accepted");
}
int main() {
    try {
        window_oracles();
        dsp_core::MagnitudeSpectrum fft, other;
        memory_oracles(fft);
        constexpr auto maximum=dsp_core::max_fft_samples;
        fft_oracles(fft,other,maximum);
        mel_oracles();
        stereo_oracles(fft);
        std::cout << "Portable DSP regressions passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
