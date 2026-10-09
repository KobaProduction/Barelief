#include "dsp_core/dsp.h"
#include <dsp_kernels/pcm_resampler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>

static bool watching = false;
static unsigned allocations = 0;
#ifdef DSP_KERNELS_WRAP_ALLOCATOR
// GNU linker wrappers also observe the vendored C backend's heap calls.
extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void *__wrap_malloc(std::size_t n) {
    if (watching) ++allocations;
    return __real_malloc(n);
}
extern "C" void *__wrap_calloc(std::size_t n, std::size_t size) {
    if (watching) ++allocations;
    return __real_calloc(n, size);
}
extern "C" void *__wrap_realloc(void *p, std::size_t n) {
    if (watching) ++allocations;
    return __real_realloc(p, n);
}
#endif
void *operator new(std::size_t n) {
    if (watching) ++allocations;
    if (auto p = std::malloc(n ? n : 1)) return p;
    std::abort();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static void finite_fft_oracles() {
    using namespace signal_contracts;
    constexpr std::size_t n=16;
    std::array<std::uint32_t,1> ids{0};
    std::array<float,n> input{};std::array<float,n/2> plain{},typed{};
    NumericContract a;a.dtype=DType::Float32;a.semantic=Semantic::Windowed;a.channel_order=ids;
    a.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{n,n},ExtentBounds{}}};
    auto b=a;b.semantic=Semantic::SpectrumMagnitude;b.shape.axes[0]=Axis::Bin;b.shape.extents[0]={n/2,n/2};
    PlaneLayout layout{0,{sizeof(float),0}};
    NumericView in{&a,std::as_bytes(std::span(input)),std::span(&layout,1),{n,1},{},{}};
    MutableNumericView out{&b,std::as_writable_bytes(std::span(typed)),std::span(&layout,1),{n/2,1},{},{}};
    dsp_core::FFT fft;check(fft.prepare(n),"extreme FFT prepare");
    for(int mode=0;mode<3;++mode){input.fill(0);const float maximum=std::numeric_limits<float>::max();
        if(mode==0)input[0]=maximum;else for(std::size_t i=0;i<n;++i)input[i]=mode==1?maximum:(i%2?-maximum:maximum);
        check(fft.process(input,plain)&&fft.process_plane(in,out,0)==Status::ok,"finite high-range FFT rejected");
        // Closed-form DFT: impulse gives |x|/N in every bin; constant gives DC;
        // alternating signal lies entirely at the excluded Nyquist bin.
        for(std::size_t k=0;k<n/2;++k){const double expected=mode==0?double(maximum)/n:mode==1&&k==0?maximum:0.;
            check(std::isfinite(plain[k])&&std::isfinite(typed[k]),"successful FFT nonfinite output");
            const double bound=double(maximum)*2e-6;
            check(std::abs(double(plain[k])-expected)<=bound&&std::abs(double(typed[k])-expected)<=bound,"independent high-range DFT oracle");
            check(plain[k]==typed[k],"plain/typed FFT parity");}
    }
    for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        input.fill(0);input[7]=bad;plain.fill(19);typed.fill(23);
        check(!fft.process(input,plain)&&fft.process_plane(in,out,0)==Status::invalid_argument,"nonfinite FFT success");
        check(std::all_of(plain.begin(),plain.end(),[](float x){return x==19;})&&std::all_of(typed.begin(),typed.end(),[](float x){return x==23;}),"FFT failure publishes partial output");}
}
static void mel_magnitude_oracle(){
    constexpr std::size_t bins=256,bands=7;std::array<float,bins> input{};std::array<float,bands> actual{};
    for(std::size_t i=0;i<bins;++i)input[i]=float(1+i%13)/17;
    dsp_core::MelFilterBank mel;check(mel.prepare(bins,48000,bands)&&mel.process(input,actual),"Mel independent oracle prepare/process");
    check(mel.workspace_bytes()>=(bands+2)*sizeof(double),"32-bit Mel boundary budget");
    std::array<double,bands+2> hz{};for(std::size_t i=0;i<hz.size();++i)hz[i]=700*(std::pow(1+24000./700,double(i)/(bands+1))-1);
    for(std::size_t m=0;m<bands;++m){double expected=0;
        for(std::size_t k=0;k<bins;++k){const double f=k*(24000./bins);
            expected+=input[k]*std::max(0.,std::min((f-hz[m])/(hz[m+1]-hz[m]),(hz[m+2]-f)/(hz[m+2]-hz[m+1])));}
        check(std::abs(actual[m]-expected)<=1e-5*std::max(1.,expected),"Mel magnitude continuous-Hz independent oracle");}
}
static void mel_frequency_and_gain_oracles() {
    // Independent closed-form triangles, sampled on the declared FFT grid.
    // Cover the low-resolution collapse that integer-rounded boundaries caused,
    // both source rates, flat spectra, moving sparse tones and linear gain.
    constexpr std::size_t bands=50;
    for (const std::size_t n : {std::size_t{512}, std::size_t{4096}}) {
        if (n>dsp_core::max_fft_samples) continue;
        for (const std::size_t rate : {std::size_t{44100}, std::size_t{48000}}) {
            dsp_core::MelFilterBank mel;
            check(mel.prepare(n/2,rate,bands),"Mel frequency matrix prepare");
            std::vector<float> spectrum(n/2),actual(bands),doubled(bands);
            std::array<double,bands+2> hz{};
            for(std::size_t i=0;i<hz.size();++i)
                hz[i]=700*(std::pow(1+double(rate)/1400,double(i)/(bands+1))-1);
            for(unsigned tick=0;tick<9;++tick) {
                std::fill(spectrum.begin(),spectrum.end(),tick==0?1.f:0.f);
                if(tick) for(unsigned tone=0;tone<8;++tone) {
                    // Include first bins and sweep the full available spectrum.
                    const auto bin=1+(tone*(n/2-2)/7+(tick-1)*(tone+1))%(n/2-1);
                    spectrum[bin]+=float(tone+1)/16;
                }
                check(mel.process(spectrum,actual),"Mel frequency matrix process");
                for(std::size_t m=0;m<bands;++m) {
                    double expected=0;
                    for(std::size_t k=0;k<spectrum.size();++k) {
                        const double frequency=double(k)*rate/n;
                        const double weight=std::max(0.,std::min(
                            (frequency-hz[m])/(hz[m+1]-hz[m]),
                            (hz[m+2]-frequency)/(hz[m+2]-hz[m+1])));
                        expected+=spectrum[k]*weight;
                    }
                    check(std::abs(actual[m]-expected)<2e-6*std::max(1.,expected),
                          "Mel FFT-size/rate independent triangle oracle");
                    if(tick==0) check(actual[m]>0,"Mel band lost to rounded-bin collapse");
                }
                for(auto& value:spectrum) value*=2;
                check(mel.process(spectrum,doubled),"Mel doubled magnitude process");
                for(std::size_t m=0;m<bands;++m)
                    check(doubled[m]==2*actual[m],"Mel silently squares or recalibrates magnitude");
            }
            // Same physical 93.75 Hz falls at bin 1/N512 or bin 8/N4096.
            // Historical floor triangles put it in band 2/N512 or band 1/N4096.
            // Fractional weights now agree across resolutions, without changing gain.
            if(rate==48000) {
                std::fill(spectrum.begin(),spectrum.end(),0.f);spectrum[n/512]=1;
                check(mel.process(spectrum,actual),"Mel physical tone process");
                check(std::abs(actual[0]-.206821f)<2e-6f &&
                      std::abs(actual[1]-.793179f)<2e-6f && actual[2]==0,
                      "Mel physical tone moved with FFT resolution");
                check(std::abs(actual[0]+actual[1]-1.f)<2e-6f,
                      "Interior Mel pair does not preserve unit tone weight");
            }
        }
    }
}
static void pcm_resampler_oracles() {
    using dsp_kernels::PcmResampler;
    check(!PcmResampler::supported(8000,192000),"unbounded resampler ratio accepted");
    check(PcmResampler::maximum_output(SIZE_MAX,8000,64000)==SIZE_MAX,"resampler capacity overflow");
    for(const auto rates:std::array<std::array<std::uint32_t,2>,5>{{{48000,48000},{48000,16000},{44100,48000},{48000,44100},{8000,64000}}}){
        constexpr std::size_t n=4096;
        const auto in=rates[0],out=rates[1];
        const auto maximum=PcmResampler::maximum_output(n,in,out);
        std::vector<float> input(n),whole(maximum),partitioned;
        for(std::size_t i=0;i<n;++i)input[i]=float(.25*std::sin(2*std::numbers::pi*437.*double(i)/in)+.125*std::cos(2*std::numbers::pi*997.*double(i)/in));
        PcmResampler full,chunked;check(full.prepare(in,out,1) && chunked.prepare(in,out,1),"resampler prepare");
        std::size_t count=0;allocations=0;watching=true;
        const bool complete=full.process(input,n,n,whole,maximum,count);watching=false;
        check(complete && allocations==0,"resampler full processing allocated");
        check(count==((n-1)*std::uint64_t(out))/in+1,"resampler output sample grid");
        whole.resize(count);partitioned.reserve(count);
        std::size_t offset=0;std::vector<float> chunk(PcmResampler::maximum_output(113,in,out));
        while(offset<n){const auto taken=std::min(n-offset,std::size_t(1+(offset*17)%113));
            std::size_t written=0;allocations=0;watching=true;
            const bool ok=chunked.process(std::span<const float>(input).subspan(offset,taken),taken,taken,chunk,chunk.size(),written);watching=false;
            check(ok && allocations==0,"resampler chunk processing allocated");
            partitioned.insert(partitioned.end(),chunk.begin(),chunk.begin()+written);offset+=taken;
        }
        check(partitioned==whole,"resampler depends on ingress partition");
        if(in==out)check(whole==input,"equal-rate resampler changed PCM");
        chunked.reset();std::size_t repeated=0;
        check(chunked.process(input,n,n,partitioned,partitioned.size(),repeated)==false,"undersized output accepted");
        std::vector<float> reset_output(maximum);check(chunked.process(input,n,n,reset_output,maximum,repeated),"reset process");
        check(repeated==whole.size() && std::equal(whole.begin(),whole.end(),reset_output.begin()),"resampler reset retained FIR samples");
        std::fill(input.begin(),input.end(),1.f);full.reset();
        check(full.process(input,n,n,reset_output,maximum,repeated),"DC process");
        for(std::size_t i=repeated-std::min(repeated,std::size_t(100));i<repeated;++i)check(std::abs(reset_output[i]-1.f)<2e-6f,"resampler DC gain");
    }
    // Independent analytic tones: a frequency above the output Nyquist must
    // not fold into a strong low-frequency signal after decimation.
    PcmResampler filter;check(filter.prepare(48000,16000,1),"alias prepare");
    std::vector<float> tone(8192),output(PcmResampler::maximum_output(tone.size(),48000,16000));
    auto energy=[&](double hz){filter.reset();for(std::size_t i=0;i<tone.size();++i)tone[i]=float(std::sin(2*std::numbers::pi*hz*i/48000.));
        std::size_t written=0;check(filter.process(tone,tone.size(),tone.size(),output,output.size(),written),"alias process");
        double total=0;for(std::size_t i=100;i<written;++i)total+=double(output[i])*output[i];return total/double(written-100);};
    check(energy(16800)<energy(1000)*.0004,"resampler aliases a rejected tone");
    PcmResampler stereo;check(stereo.prepare(48000,48000,2),"stereo prepare");
    std::array<float,8> channels{1,2,3,4,-1,-2,-3,-4};std::array<float,10> result{};std::size_t written=0;
    check(stereo.process(channels,4,4,result,5,written) && written==4 && result[0]==1 && result[5]==-1 && result[8]==-4,"resampler channel isolation");
}

int main() { pcm_resampler_oracles();finite_fft_oracles();mel_magnitude_oracle();mel_frequency_and_gain_oracles();
#ifdef DSP_KERNELS_WRAP_ALLOCATOR
    // Verify the C linker guard itself before relying on a zero count.
    void *(*volatile probe)(std::size_t) = std::malloc;
    watching = true;
    auto *probe_memory = probe(8);
    watching = false;
    std::free(probe_memory);
    check(allocations > 0, "C allocation guard is inactive");
    allocations = 0;
#endif
    // Eight changing silent tones, both FFT radices, every supported profile.
    for (std::size_t n = 512; n <= dsp_core::max_fft_samples; n *= 2) {
        dsp_core::StereoProcessor stereo;
        dsp_core::FFT reference;
        dsp_core::MelFilterBank mel;
        check(stereo.prepare(n, 48000, 50) && reference.prepare(n, (n & 0x55555555) != 0) &&
                  mel.prepare(n / 2, 48000, 50), "prepare failed");
        std::vector<float> left(n), right(n), expected_left(n), expected_right(n), win(n), weights(n / 2);
        std::vector<float> sl(n / 2), sr(n / 2), expected_sl(n / 2), expected_sr(n / 2);
        std::vector<float> ml(50), mr(50), expected_ml(50), expected_mr(50);
        dsp_core::hann_window(win);
        dsp_core::bark_weights(weights, 48000.f / n);
        const auto workspace = stereo.workspace_bytes();
        for (unsigned tick = 0; tick < 5; ++tick) {
            for (std::size_t i = 0; i < n; ++i) {
                float x = 0, y = 0;
                for (unsigned tone = 0; tone < 8; ++tone) {
                    const double phase = 2 * std::numbers::pi * (7 + tone * 11 + tick * .25) * i / n;
                    x += float(std::sin(phase)) * (.02f * (tone + 1));
                    y += float(std::cos(phase)) * (.01f * (8 - tone));
                }
                left[i] = x; right[i] = tick == 1 ? -x : tick == 2 ? 0 : y;
                expected_left[i] = left[i] * win[i]; expected_right[i] = right[i] * win[i];
            }
            check(reference.process(expected_left, expected_sl) && reference.process(expected_right, expected_sr),
                  "reference FFT failed");
            for (std::size_t i = 0; i < sl.size(); ++i) {
                expected_sl[i] *= weights[i]; expected_sr[i] *= weights[i];
            }
            check(mel.process(expected_sl, expected_ml) && mel.process(expected_sr, expected_mr), "reference Mel failed");
            watching = true;
            const bool ok = stereo.process(left, right, win, weights, sl, sr, ml, mr);
            watching = false;
            check(ok && allocations == 0, "full prepared stereo kernel allocated or failed");
            check(left == expected_left && right == expected_right && sl == expected_sl && sr == expected_sr &&
                      ml == expected_ml && mr == expected_mr, "stereo numerical order or channel behavior changed");
            check(stereo.workspace_bytes() == workspace, "workspace changed during processing");
            // FFT magnitude and Hartley routes and Mel-only route remain allocation-free.
            watching = true;
            const bool extra = reference.process(left, sl, true) && stereo.process_mel(sl, sr, ml, mr);
            watching = false;
            check(extra && allocations == 0, "Hartley or Mel-only kernel allocated");
        }
        std::fill(weights.begin(), weights.end(), std::numeric_limits<float>::infinity());
        check(stereo.process(left, right, win, weights, sl, sr, ml, mr), "nonfinite weight process failed");
        check(std::all_of(sl.begin(), sl.end(), [](float x) { return x == 0; }) &&
                  std::all_of(sr.begin(), sr.end(), [](float x) { return x == 0; }), "nonfinite spectra sanitization changed");
    }
    std::cout << "Full stereo/FFT/Hartley/Mel characterization and allocation guard passed\n";
}
