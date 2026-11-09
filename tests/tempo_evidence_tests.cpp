#include <dsp_kernels/tempo_evidence.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/peaks.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
using namespace dsp_kernels;
using namespace dsp_kernels::rhythm;
std::size_t allocations=0;
constexpr double tau=6.2831853071795864769;
constexpr auto long_fft=std::min<std::size_t>(4096,max_fft_samples);
TempoEvidenceConfig evidence_config() {
    TempoEvidenceConfig config;
    config.long_fft=long_fft;
    return config;
}
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
struct Curve {
    std::size_t size;
    std::array<float,4096> values{},windowed{};
    std::array<float,2048> spectrum{};
    std::array<float,121> acf{};
    explicit Curve(std::size_t n):size(n){}
    void generate(double bpm) {
        double mean=0;
        for(std::size_t i=0;i<size;++i){
            double value=.5+.3*std::cos(tau*bpm/60*double(i)/60);
            for(unsigned tone=0;tone<4;++tone)value+=.02*std::cos(tau*(4.3+tone*2.1)*double(i)/60+.17*tone);
            values[i]=float(value);check(value>=0 && value<=1,"five modulation tones stay normalized");mean+=values[i];
        }
        for(std::size_t i=0;i<size;++i){values[i]=float(double(values[i])-mean/double(size));windowed[i]=values[i]*float(.5-.5*std::cos(tau*double(i)/double(size-1)));}
        MagnitudeSpectrum fft;check(fft.prepare(size),"prepare existing Fourier kernel");
        check(fft.process(std::span(windowed).first(size),std::span(spectrum).first(size/2)),"same Fourier on centered Hann history");
        check(linear_autocorrelation(std::span(values).first(size),acf),"same linear ACF on centered history");
    }
    PeriodicityCurve view(std::size_t filled) const {return {std::span(spectrum).first(size/2),acf,filled};}
};
void known_tempi_and_scaling() {
    Curve short_curve(512),long_curve(long_fft);PreparedTempoEvidence kernel;check(kernel.prepare(evidence_config()),"prepare evidence");
    for(double bpm:{90.,110.,120.,200.,215.}){
        short_curve.generate(bpm);long_curve.generate(bpm);const auto before=allocations;
        check(kernel.process(short_curve.view(512),long_curve.view(long_fft)),"finite histories accepted");
        check(allocations==before,"evidence processing never allocates");
        const auto original=kernel.output();check(original.count>0 && original.count<=8,"bounded candidate set");
        if(std::abs(original.candidates[0].bpm/bpm-1)>.02)std::fprintf(stderr,"expected %.3f found %.3f\n",bpm,original.candidates[0].bpm);
        check(std::abs(original.candidates[0].bpm/bpm-1)<=.02,"isolated dominant modulation resolves off-bin and integer-lag quantization");
        for(auto& value:short_curve.spectrum)value*=1e-12f;
        for(auto& value:long_curve.spectrum)value*=1e-12f;
        check(kernel.process(short_curve.view(512),long_curve.view(long_fft)),"very weak finite spectra accepted");
        check(std::abs(kernel.output().candidates[0].bpm-original.candidates[0].bpm)<1e-5,"ranking independent of amplitude units");
        check(std::abs(kernel.output().candidates[0].score-original.candidates[0].score)<1e-6,"bounded support is scale invariant");
    }
}
void warming_silence_and_atomic_errors() {
    Curve s(512),l(long_fft);s.generate(110);l.generate(110);PreparedTempoEvidence kernel;check(kernel.prepare(evidence_config()),"prepare warming");
    check(kernel.process(s.view(239),l.view(239)) && kernel.output().count==0,"less than explicit four seconds remains unavailable");
    check(kernel.process(s.view(480),l.view(480)) && kernel.output().count>0,"partial long history becomes evidence after enough real samples");
    const auto before=kernel.output();l.spectrum[100]=std::numeric_limits<float>::infinity();
    check(!kernel.process(s.view(512),l.view(long_fft)) && kernel.output().candidates[0].bpm==before.candidates[0].bpm,"invalid tail rejected before replacing output");
    check(!kernel.prepare({0}) && kernel.output().count==before.count,"invalid prepare preserves accepted state");
    l.spectrum.fill(0);s.spectrum.fill(0);l.acf.fill(0);s.acf.fill(0);
    check(kernel.process(s.view(512),l.view(long_fft)) && kernel.output().count==0,"silence does not produce a tempo");
    auto invalid=l.view(long_fft);invalid.filled=long_fft+1;
    check(!kernel.process(s.view(512),invalid),"impossible occupancy refused");
}
void ambiguity_and_interpolation() {
    check(std::abs(quadratic_peak_offset(1,4,3)-.25)<1e-12,"independent three-point parabola vertex");
    check(quadratic_peak_offset(1,1,1)==0 && quadratic_peak_offset(3,1,3)==0,"flat and convex triples do not invent an interior maximum");
    Curve s(512),l(long_fft);
    for(auto* curve:{&s,&l}){
        curve->acf[0]=1;curve->acf[18]=curve->acf[36]=1;
        curve->spectrum[std::size_t(std::llround(100.*double(curve->size)/3600))]=1;
        curve->spectrum[std::size_t(std::llround(200.*double(curve->size)/3600))]=1;
    }
    PreparedTempoEvidence kernel;check(kernel.prepare(evidence_config()) && kernel.process(s.view(512),l.view(long_fft)),"two metrical levels accepted as evidence");
    bool slow=false,fast=false;
    for(std::size_t i=0;i<kernel.output().count;++i){
        const auto& value=kernel.output().candidates[i];slow=slow || std::abs(value.bpm/100-1)<.02;fast=fast || std::abs(value.bpm/200-1)<.02;
        check(value.score>=0 && value.score<=1 && value.origin_curves>0,"bounded support with correlated-curve provenance");
    }
    check(slow && fast && kernel.output().harmonic_ambiguity,"keep octave alternatives and explicit ambiguity");
    check(kernel.output().truncated_proposals==0,"fixed proposal capacity covers all configured peak families");
}
void bar_level_ambiguity() {
    for(const double fast:{180.,240.}){
        Curve s(512),l(long_fft);
        for(auto* curve:{&s,&l}){
            curve->acf[0]=curve->acf[60]=curve->acf[std::size_t(std::llround(3600/fast))]=1;
            curve->spectrum[std::size_t(std::llround(60.*double(curve->size)/3600))]=1;
            curve->spectrum[std::size_t(std::llround(fast*double(curve->size)/3600))]=1;
        }
        PreparedTempoEvidence kernel;
        check(kernel.prepare(evidence_config()) && kernel.process(s.view(512),l.view(long_fft)),"bar/beat evidence processed");
        check(kernel.output().harmonic_ambiguity,"three- and four-beat bar periods cannot look unambiguous");
        check(kernel.output().truncated_proposals==0,"all nine relatives fit bounded storage");
    }
}
}
void* operator new(std::size_t size){++allocations;auto* p=std::malloc(size?size:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(){known_tempi_and_scaling();warming_silence_and_atomic_errors();ambiguity_and_interpolation();bar_level_ambiguity();std::puts("refined tempo evidence off-bin/scale/partial/silence/octave/bar/atomic/no-allocation PASS");}
